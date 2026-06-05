
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <microhttpd.h>
#include <sqlite3.h>

#include "appdb.h"
#include "ps5/http.h"
#include "titleid.h"
#include "websrv.h"

#include "third_party/stb_image.h"
#include "third_party/stb_image_write.h"


#define ICON_MAX_DIM   256
#define ICON_JPEG_Q    80


static int
json_extract_string(const char *json, const char *key, char *out, size_t out_size) {
  if(!json || !key || !out || out_size < 2) {
    return 0;
  }
  char needle[64];
  snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char *p = strstr(json, needle);
  if(!p) return 0;
  p += strlen(needle);
  /* skip whitespace and a single ':' */
  while(*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
  if(*p != ':') return 0;
  p++;
  while(*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
  if(*p != '"') return 0;
  p++;
  size_t i = 0;
  while(*p && *p != '"' && i < out_size-1) {
    if(*p == '\\' && p[1]) {
      /* tiny unescape: \\ \" \n \t */
      switch(p[1]) {
        case 'n': out[i++] = '\n'; break;
        case 't': out[i++] = '\t'; break;
        case 'r': out[i++] = '\r'; break;
        case '"': out[i++] = '"'; break;
        case '\\': out[i++] = '\\'; break;
        case '/': out[i++] = '/'; break;
        default:  out[i++] = p[1]; break;
      }
      p += 2;
    } else {
      out[i++] = *p++;
    }
  }
  out[i] = 0;
  return i > 0 ? 1 : 0;
}


static int
read_title_name_from_appinfo(sqlite3 *db, const char *id, char *out,
                             size_t out_size) {
  /* TITLE_01 is en-US in PS5 firmware. Order: en-US first, then any other
     localized variant we have, then the language-neutral 'TITLE' fallback. */
  static const char *prefer_keys[] = {
    "TITLE_01", "TITLE_02", "TITLE_03", "TITLE_04", "TITLE_05",
    "TITLE_06", "TITLE_07", "TITLE_08", "TITLE_09", "TITLE_10",
    "TITLE_11", "TITLE_12", "TITLE_13", "TITLE_14", "TITLE_15",
    "TITLE_16", "TITLE_17", "TITLE_18", "TITLE_19", "TITLE_20",
    "TITLE_21", "TITLE_22", "TITLE_00", "TITLE", NULL,
  };

  out[0] = 0;
  for(int i=0; prefer_keys[i]; i++) {
    sqlite3_stmt *st = NULL;
    const char *sql =
      "SELECT val FROM tbl_appinfo "
      "WHERE titleId=?1 AND key=?2 AND val IS NOT NULL AND val != '' "
      "LIMIT 1;";
    if(sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
      return 0;
    }
    sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, prefer_keys[i], -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    if(rc == SQLITE_ROW) {
      const unsigned char *v = sqlite3_column_text(st, 0);
      if(v && *v) {
        strncpy(out, (const char*)v, out_size-1);
        out[out_size-1] = 0;
        sqlite3_finalize(st);
        return 1;
      }
    }
    sqlite3_finalize(st);
  }
  return 0;
}


static int
read_title_name(const char *id, char *out, size_t out_size) {
  static const char *candidates[] = {
    "/user/appmeta/%s/param.json",
    "/user/appmeta/%s/sce_sys/param.json",
    "/user/app/%s/sce_sys/param.json",
    NULL,
  };

  out[0] = 0;
  for(int i=0; candidates[i]; i++) {
    char path[256];
    snprintf(path, sizeof(path), candidates[i], id);

    struct stat st;
    if(stat(path, &st) != 0) continue;
    if(st.st_size <= 0 || st.st_size > 1024*1024) continue;

    int fd = open(path, O_RDONLY);
    if(fd < 0) continue;

    char *buf = malloc((size_t)st.st_size + 1);
    if(!buf) { close(fd); continue; }
    ssize_t n = read(fd, buf, (size_t)st.st_size);
    close(fd);
    if(n != (ssize_t)st.st_size) { free(buf); continue; }
    buf[n] = 0;

    /* Prefer en-US localized title; if missing, defaultLanguage block;
       if missing, the first titleName found anywhere. */
    char *en_block = strstr(buf, "\"en-US\"");
    if(en_block) {
      char *brace = strchr(en_block, '{');
      char *end = brace ? strchr(brace, '}') : NULL;
      if(brace && end) {
        char saved = *end;
        *end = 0;
        if(json_extract_string(brace, "titleName", out, out_size)) {
          *end = saved;
          free(buf);
          return 1;
        }
        *end = saved;
      }
    }

    char def_lang[32] = {0};
    if(json_extract_string(buf, "defaultLanguage", def_lang, sizeof(def_lang))) {
      char needle[64];
      snprintf(needle, sizeof(needle), "\"%s\"", def_lang);
      char *blk = strstr(buf, needle);
      if(blk) {
        char *brace = strchr(blk, '{');
        char *end = brace ? strchr(brace, '}') : NULL;
        if(brace && end) {
          char saved = *end;
          *end = 0;
          if(json_extract_string(brace, "titleName", out, out_size)) {
            *end = saved;
            free(buf);
            return 1;
          }
          *end = saved;
        }
      }
    }

    if(json_extract_string(buf, "titleName", out, out_size)) {
      free(buf);
      return 1;
    }

    free(buf);
  }
  return 0;
}


#define APP_DB_PATH "/system_data/priv/mms/app.db"


static int
is_safe_title_id(const char *s) {
  if(!s || !*s) {
    return 0;
  }
  for(const char *p=s; *p; p++) {
    char c = *p;
    int ok = (c>='A' && c<='Z') || (c>='a' && c<='z') ||
             (c>='0' && c<='9') || c=='-' || c=='_';
    if(!ok) {
      return 0;
    }
  }
  return strlen(s) < 24;
}


static char*
json_escape(const char *s) {
  if(!s) {
    return strdup("");
  }
  size_t len = strlen(s);
  char *out = malloc(len*6 + 1);
  char *w = out;

  for(size_t i=0; i<len; i++) {
    unsigned char c = (unsigned char)s[i];
    switch(c) {
      case '"':  *w++='\\'; *w++='"'; break;
      case '\\': *w++='\\'; *w++='\\'; break;
      case '\b': *w++='\\'; *w++='b'; break;
      case '\f': *w++='\\'; *w++='f'; break;
      case '\n': *w++='\\'; *w++='n'; break;
      case '\r': *w++='\\'; *w++='r'; break;
      case '\t': *w++='\\'; *w++='t'; break;
      default:
        if(c < 0x20) {
          w += sprintf(w, "\\u%04x", c);
        } else {
          *w++ = (char)c;
        }
    }
  }
  *w = 0;
  return out;
}


static enum MHD_Result
serve_buffer(struct MHD_Connection *conn, unsigned int status,
             const char *mime, void *data, size_t size, int free_after) {
  enum MHD_Result ret = MHD_NO;
  struct MHD_Response *resp;
  enum MHD_ResponseMemoryMode mode = free_after ? MHD_RESPMEM_MUST_FREE
                                                : MHD_RESPMEM_PERSISTENT;

  if((resp=MHD_create_response_from_buffer(size, data, mode))) {
    if(mime) {
      MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, mime);
    }
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-cache");
    ret = websrv_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
  } else if(free_after) {
    free(data);
  }

  return ret;
}


typedef struct {
  char  *buf;
  size_t cap;
  size_t len;
} strbuf_t;


static void
sb_init(strbuf_t *sb) {
  sb->cap = 4096;
  sb->len = 0;
  sb->buf = malloc(sb->cap);
  sb->buf[0] = 0;
}


static void
sb_grow(strbuf_t *sb, size_t need) {
  if(sb->len + need + 1 <= sb->cap) {
    return;
  }
  while(sb->len + need + 1 > sb->cap) {
    sb->cap *= 2;
  }
  sb->buf = realloc(sb->buf, sb->cap);
}


static void
sb_putc(strbuf_t *sb, char c) {
  sb_grow(sb, 1);
  sb->buf[sb->len++] = c;
  sb->buf[sb->len] = 0;
}


static void
sb_puts(strbuf_t *sb, const char *s) {
  size_t n = strlen(s);
  sb_grow(sb, n);
  memcpy(sb->buf+sb->len, s, n);
  sb->len += n;
  sb->buf[sb->len] = 0;
}


static void
sb_puts_json(strbuf_t *sb, const char *s) {
  char *e = json_escape(s);
  sb_puts(sb, e);
  free(e);
}


static int
table_has_column(sqlite3 *db, const char *table, const char *column) {
  char sql[256];
  sqlite3_stmt *st = NULL;
  int found = 0;

  snprintf(sql, sizeof(sql), "PRAGMA table_info(\"%s\")", table);
  if(sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
    return 0;
  }
  while(sqlite3_step(st) == SQLITE_ROW) {
    const unsigned char *name = sqlite3_column_text(st, 1);
    if(name && !strcmp((const char*)name, column)) {
      found = 1;
      break;
    }
  }
  sqlite3_finalize(st);
  return found;
}


static int
table_exists(sqlite3 *db, const char *table) {
  sqlite3_stmt *st = NULL;
  int found = 0;
  const char *sql =
    "SELECT name FROM sqlite_master WHERE type='table' AND name=?1;";

  if(sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
    return 0;
  }
  sqlite3_bind_text(st, 1, table, -1, SQLITE_TRANSIENT);
  if(sqlite3_step(st) == SQLITE_ROW) {
    found = 1;
  }
  sqlite3_finalize(st);
  return found;
}


static enum MHD_Result
appdb_list_request(struct MHD_Connection *conn) {
  sqlite3 *db = NULL;
  sqlite3_stmt *st = NULL;
  strbuf_t sb;
  int first = 1;

  sb_init(&sb);

  if(sqlite3_open_v2(APP_DB_PATH, &db,
                     SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX,
                     NULL) != SQLITE_OK) {
    sb_puts(&sb,
      "{\"error\":\"Unable to open app.db. Make sure kstuff is loaded so the"
      " payload has access to /system_data/priv/mms/app.db.\"}");
    if(db) sqlite3_close(db);
    return serve_buffer(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                        "application/json", sb.buf, sb.len, 1);
  }

  sqlite3_busy_timeout(db, 3000);

  const char *sql = NULL;
  int has_appbrowse = table_exists(db, "tbl_appbrowse");
  int has_name = has_appbrowse && table_has_column(db, "tbl_appbrowse",
                                                   "titleName");
  int has_sort = has_appbrowse && table_has_column(db, "tbl_appbrowse",
                                                   "sortPriority");

  /* Only surface real game titles. PS5 prefixes are CUSA (PS4-compat) and
     PPSA (PS5 native); NPXS/FAKE/etc. are system shells / shortcuts. */
  if(has_appbrowse && has_name && has_sort) {
    sql = "SELECT DISTINCT titleId, COALESCE(titleName,'') AS n, "
          "       COALESCE(sortPriority,0) AS p "
          "FROM tbl_appbrowse "
          "WHERE titleId IS NOT NULL AND titleId != ''"
          " AND substr(titleId,1,4) IN "
          "  ('CUSA','PPSA',"                             /* PS4, PS5 */
          "   'ULUS','ULES','ULJS','ULKS',"               /* PSP */
          "   'SLUS','SCUS','SLES','SCES',"               /* PS1/PS2 */
          "   'SLPS','SLPM','SCED','SLED','SCPS') "
          "ORDER BY n COLLATE NOCASE, titleId;";
  } else if(has_appbrowse && has_name) {
    sql = "SELECT DISTINCT titleId, COALESCE(titleName,'') AS n, 0 AS p "
          "FROM tbl_appbrowse "
          "WHERE titleId IS NOT NULL AND titleId != ''"
          " AND substr(titleId,1,4) IN "
          "  ('CUSA','PPSA',"                             /* PS4, PS5 */
          "   'ULUS','ULES','ULJS','ULKS',"               /* PSP */
          "   'SLUS','SCUS','SLES','SCES',"               /* PS1/PS2 */
          "   'SLPS','SLPM','SCED','SLED','SCPS') "
          "ORDER BY n COLLATE NOCASE, titleId;";
  } else if(has_appbrowse) {
    sql = "SELECT DISTINCT titleId, '' AS n, 0 AS p "
          "FROM tbl_appbrowse "
          "WHERE titleId IS NOT NULL AND titleId != ''"
          " AND substr(titleId,1,4) IN "
          "  ('CUSA','PPSA',"                             /* PS4, PS5 */
          "   'ULUS','ULES','ULJS','ULKS',"               /* PSP */
          "   'SLUS','SCUS','SLES','SCES',"               /* PS1/PS2 */
          "   'SLPS','SLPM','SCED','SLED','SCPS') "
          "ORDER BY titleId;";
  } else {
    int has_ctitle = table_has_column(db, "tbl_contentinfo", "titleName");
    if(has_ctitle) {
      sql = "SELECT DISTINCT titleId, COALESCE(titleName,'') AS n, 0 AS p "
            "FROM tbl_contentinfo "
            "WHERE titleId IS NOT NULL AND titleId != ''"
            " AND substr(titleId,1,4) IN "
          "  ('CUSA','PPSA',"                             /* PS4, PS5 */
          "   'ULUS','ULES','ULJS','ULKS',"               /* PSP */
          "   'SLUS','SCUS','SLES','SCES',"               /* PS1/PS2 */
          "   'SLPS','SLPM','SCED','SLED','SCPS') "
            "ORDER BY n COLLATE NOCASE, titleId;";
    } else {
      sql = "SELECT DISTINCT titleId, '' AS n, 0 AS p "
            "FROM tbl_contentinfo "
            "WHERE titleId IS NOT NULL AND titleId != ''"
            " AND substr(titleId,1,4) IN "
          "  ('CUSA','PPSA',"                             /* PS4, PS5 */
          "   'ULUS','ULES','ULJS','ULKS',"               /* PSP */
          "   'SLUS','SCUS','SLES','SCES',"               /* PS1/PS2 */
          "   'SLPS','SLPM','SCED','SLED','SCPS') "
            "ORDER BY titleId;";
    }
  }

  if(sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
    sb_puts(&sb, "{\"error\":\"prepare failed: ");
    sb_puts_json(&sb, sqlite3_errmsg(db));
    sb_puts(&sb, "\"}");
    sqlite3_close(db);
    return serve_buffer(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                        "application/json", sb.buf, sb.len, 1);
  }

  sb_puts(&sb, "{\"titles\":[");
  while(sqlite3_step(st) == SQLITE_ROW) {
    const unsigned char *id   = sqlite3_column_text(st, 0);
    const unsigned char *name = sqlite3_column_text(st, 1);
    int sort = sqlite3_column_int(st, 2);

    if(!id) continue;

    char fallback_name[256];
    fallback_name[0] = 0;
    if(!name || !*name) {
      if(read_title_name_from_appinfo(db, (const char*)id, fallback_name,
                                      sizeof(fallback_name))) {
        name = (const unsigned char*)fallback_name;
      } else if(read_title_name((const char*)id, fallback_name,
                                sizeof(fallback_name))) {
        name = (const unsigned char*)fallback_name;
      }
    }

    if(!first) sb_putc(&sb, ',');
    first = 0;

    sb_puts(&sb, "{\"titleId\":\"");
    sb_puts_json(&sb, (const char*)id);
    sb_puts(&sb, "\",\"titleName\":\"");
    sb_puts_json(&sb, (const char*)(name ? name : (const unsigned char*)""));
    sb_puts(&sb, "\",\"sortPriority\":");
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%d", sort);
    sb_puts(&sb, tmp);
    sb_putc(&sb, '}');
  }
  sb_puts(&sb, "]}");

  sqlite3_finalize(st);
  sqlite3_close(db);

  return serve_buffer(conn, MHD_HTTP_OK, "application/json",
                      sb.buf, sb.len, 1);
}


/* Box-filter downscale of an RGBA buffer. Produces dst_w*dst_h*4 bytes. */
static void
icon_downscale_rgba(const unsigned char *src, int sw, int sh,
                    unsigned char *dst, int dw, int dh) {
  for(int y=0; y<dh; y++) {
    int sy0 = y * sh / dh;
    int sy1 = (y+1) * sh / dh;
    if(sy1 <= sy0) sy1 = sy0 + 1;
    for(int x=0; x<dw; x++) {
      int sx0 = x * sw / dw;
      int sx1 = (x+1) * sw / dw;
      if(sx1 <= sx0) sx1 = sx0 + 1;
      uint32_t r=0, g=0, b=0;
      uint32_t n = 0;
      for(int yy=sy0; yy<sy1; yy++) {
        const unsigned char *row = src + (yy*sw + sx0) * 4;
        for(int xx=sx0; xx<sx1; xx++) {
          r += row[0];
          g += row[1];
          b += row[2];
          row += 4;
          n++;
        }
      }
      unsigned char *p = dst + (y*dw + x) * 3;
      p[0] = r / n;
      p[1] = g / n;
      p[2] = b / n;
    }
  }
}


typedef struct {
  unsigned char *buf;
  size_t        len;
  size_t        cap;
} jpeg_sink_t;


static void
jpeg_sink_write(void *cls, void *data, int size) {
  jpeg_sink_t *s = cls;
  if(s->len + size > s->cap) {
    s->cap = (s->len + size) * 2;
    s->buf = realloc(s->buf, s->cap);
  }
  memcpy(s->buf + s->len, data, size);
  s->len += size;
}


static int
resolve_icon_path_from_appdb(const char *id, char *out, size_t out_size) {
  sqlite3 *db = NULL;
  int rc = 0;

  if(sqlite3_open_v2(APP_DB_PATH, &db,
                     SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX,
                     NULL) != SQLITE_OK) {
    if(db) sqlite3_close(db);
    return 0;
  }
  if(!table_has_column(db, "tbl_contentinfo", "icon0Info")) {
    sqlite3_close(db);
    return 0;
  }
  sqlite3_stmt *st = NULL;
  const char *sql =
    "SELECT icon0Info FROM tbl_contentinfo "
    "WHERE titleId=?1 AND icon0Info IS NOT NULL AND icon0Info != '' "
    "LIMIT 1;";
  if(sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
    sqlite3_close(db);
    return 0;
  }
  sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
  if(sqlite3_step(st) == SQLITE_ROW) {
    const unsigned char *v = sqlite3_column_text(st, 0);
    if(v && *v) {
      /* Copy up to and including the LAST ".png" — guards against an
         icon name that just happens to contain ".png" mid-string. */
      const char *s = (const char*)v;
      const char *p = strstr(s, ".png");
      const char *cut = p;
      if(p) {
        const char *next;
        while((next = strstr(p + 4, ".png")) != NULL) {
          cut = next;
          p = next;
        }
      }
      if(cut) {
        size_t n = (size_t)(cut - s) + 4;  /* keep the ".png" itself */
        if(n >= out_size) n = out_size - 1;
        memcpy(out, s, n);
        out[n] = 0;
        struct stat st_;
        if(stat(out, &st_) == 0) rc = 1;
      }
    }
  }
  sqlite3_finalize(st);
  sqlite3_close(db);
  return rc;
}


static int
build_icon_jpeg(const char *id, unsigned char **out_buf, size_t *out_len) {
  char path[256];
  unsigned char *png_buf = NULL;
  size_t png_len = 0;
  int w = 0, h = 0, c = 0;
  unsigned char *rgba = NULL;
  unsigned char *out_rgb = NULL;
  jpeg_sink_t sink = {0};
  int rc = 0;

  if(!resolve_icon_path_from_appdb(id, path, sizeof(path))) {
    snprintf(path, sizeof(path), "/user/appmeta/%s/icon0.png", id);
  }

  /* Slurp the PNG into memory. */
  {
    struct stat st;
    if(stat(path, &st) != 0) goto done;
    int fd = open(path, O_RDONLY);
    if(fd < 0) goto done;
    png_len = st.st_size;
    png_buf = malloc(png_len);
    if(!png_buf) { close(fd); goto done; }
    if(read(fd, png_buf, png_len) != (ssize_t)png_len) {
      close(fd);
      goto done;
    }
    close(fd);
  }

  rgba = stbi_load_from_memory(png_buf, (int)png_len, &w, &h, &c, 4);
  if(!rgba || w <= 0 || h <= 0) goto done;

  int dw = w;
  int dh = h;
  if(w > ICON_MAX_DIM || h > ICON_MAX_DIM) {
    if(w >= h) {
      dw = ICON_MAX_DIM;
      dh = (h * ICON_MAX_DIM + w/2) / w;
      if(dh < 1) dh = 1;
    } else {
      dh = ICON_MAX_DIM;
      dw = (w * ICON_MAX_DIM + h/2) / h;
      if(dw < 1) dw = 1;
    }
  }

  out_rgb = malloc((size_t)dw * (size_t)dh * 3);
  if(!out_rgb) goto done;

  if(dw == w && dh == h) {
    /* No downscale: just strip alpha. */
    for(int i=0; i<w*h; i++) {
      out_rgb[i*3+0] = rgba[i*4+0];
      out_rgb[i*3+1] = rgba[i*4+1];
      out_rgb[i*3+2] = rgba[i*4+2];
    }
  } else {
    icon_downscale_rgba(rgba, w, h, out_rgb, dw, dh);
  }

  sink.cap = 32 * 1024;
  sink.buf = malloc(sink.cap);
  if(!sink.buf) goto done;

  if(!stbi_write_jpg_to_func(jpeg_sink_write, &sink, dw, dh, 3, out_rgb,
                             ICON_JPEG_Q)) {
    free(sink.buf);
    sink.buf = NULL;
    goto done;
  }

  *out_buf = sink.buf;
  *out_len = sink.len;
  rc = 1;
  sink.buf = NULL; /* transferred to caller */

done:
  free(png_buf);
  if(rgba) stbi_image_free(rgba);
  free(out_rgb);
  free(sink.buf);
  return rc;
}


static enum MHD_Result
appdb_icon_request(struct MHD_Connection *conn) {
  const char *id = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                               "id");
  unsigned char *jpeg = NULL;
  size_t jpeg_len = 0;
  enum MHD_Result ret;
  struct MHD_Response *resp;

  if(!id || !is_safe_title_id(id)) {
    return serve_buffer(conn, MHD_HTTP_BAD_REQUEST, "text/plain",
                        "bad id", 6, 0);
  }

  if(!build_icon_jpeg(id, &jpeg, &jpeg_len)) {
    return serve_buffer(conn, MHD_HTTP_NOT_FOUND, "text/plain",
                        "no icon", 7, 0);
  }

  resp = MHD_create_response_from_buffer(jpeg_len, jpeg, MHD_RESPMEM_MUST_FREE);
  if(!resp) {
    free(jpeg);
    return serve_buffer(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "text/plain",
                        "alloc", 5, 0);
  }

  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "image/jpeg");
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL,
                          "public, max-age=86400");
  ret = websrv_queue_response(conn, MHD_HTTP_OK, resp);
  MHD_destroy_response(resp);
  return ret;
}


static int
is_title_id(const char *s) {
  return title_id_normalize(s, NULL);
}

#define PIC0_MAX_W   960
#define PIC0_JPEG_Q   72   /* slightly lower q than icon to save bandwidth */

/* Replace the last occurrence of "icon0.png" in `src` with `repl` and
   write the result into `out` (out_size bytes). Returns 1 on success. */
static int
subst_icon0(const char *src, const char *repl, char *out, size_t out_size) {
  /* Find the last "icon0.png" — guards against a path that contains the
     substring in a directory component. */
  const char *needle = "icon0.png";
  size_t nlen = strlen(needle);
  const char *p = NULL, *q = src;
  while ((q = strstr(q, needle)) != NULL) { p = q; q += nlen; }
  if (!p) return 0;   /* no "icon0.png" in path */

  size_t prefix = (size_t)(p - src);
  size_t rlen   = strlen(repl);
  if (prefix + rlen >= out_size) return 0;

  memcpy(out, src, prefix);
  memcpy(out + prefix, repl, rlen);
  out[prefix + rlen] = 0;
  return 1;
}

/* Resolve a pic0.png path for `id`.  Returns 1 and fills `out` when a
   readable file is found; 0 otherwise. */
static int
resolve_pic0_path(const char *id, char *out, size_t out_size) {
  char icon_path[256];

  /* Get the icon0 base path — reuse the existing resolver so we
     automatically handle /user/appmeta/external/… titles. */
  if (!resolve_icon_path_from_appdb(id, icon_path, sizeof(icon_path))) {
    /* Fallback: plain /user/appmeta/<id>/icon0.png */
    snprintf(icon_path, sizeof(icon_path),
             "/user/appmeta/%s/icon0.png", id);
  }

  /* Candidate 1: same directory, pic0.png */
  if (subst_icon0(icon_path, "pic0.png", out, out_size)) {
    struct stat st;
    if (stat(out, &st) == 0 && st.st_size > 0) return 1;
  }

  /* Candidate 2: sce_sys/ subdirectory */
  if (subst_icon0(icon_path, "sce_sys/pic0.png", out, out_size)) {
    struct stat st;
    if (stat(out, &st) == 0 && st.st_size > 0) return 1;
  }

  out[0] = 0;
  return 0;
}


static int
build_pic0_jpeg(const char *id, unsigned char **out_buf, size_t *out_len) {
  char path[256];
  unsigned char *file_buf = NULL;
  size_t file_len = 0;
  int w = 0, h = 0, c = 0;
  unsigned char *rgba = NULL;
  unsigned char *out_rgb = NULL;
  jpeg_sink_t sink = {0};
  int rc = 0;

  if (!resolve_pic0_path(id, path, sizeof(path))) return 0;

  /* Slurp the PNG into memory. */
  {
    struct stat st;
    if (stat(path, &st) != 0) goto pic0_done;
    if (st.st_size <= 0 || st.st_size > 32 * 1024 * 1024) goto pic0_done;
    int fd = open(path, O_RDONLY);
    if (fd < 0) goto pic0_done;
    file_len = (size_t)st.st_size;
    file_buf = malloc(file_len);
    if (!file_buf) { close(fd); goto pic0_done; }
    if (read(fd, file_buf, file_len) != (ssize_t)file_len) {
      close(fd);
      goto pic0_done;
    }
    close(fd);
  }

  rgba = stbi_load_from_memory(file_buf, (int)file_len, &w, &h, &c, 4);
  if (!rgba || w <= 0 || h <= 0) goto pic0_done;

  /* Downscale to at most PIC0_MAX_W wide, preserving aspect ratio. */
  int dw = w, dh = h;
  if (w > PIC0_MAX_W) {
    dw = PIC0_MAX_W;
    dh = (h * PIC0_MAX_W + w / 2) / w;
    if (dh < 1) dh = 1;
  }

  out_rgb = malloc((size_t)dw * (size_t)dh * 3);
  if (!out_rgb) goto pic0_done;

  if (dw == w && dh == h) {
    for (int i = 0; i < w * h; i++) {
      out_rgb[i*3+0] = rgba[i*4+0];
      out_rgb[i*3+1] = rgba[i*4+1];
      out_rgb[i*3+2] = rgba[i*4+2];
    }
  } else {
    icon_downscale_rgba(rgba, w, h, out_rgb, dw, dh);
  }

  sink.cap = 256 * 1024;
  sink.buf = malloc(sink.cap);
  if (!sink.buf) goto pic0_done;

  if (!stbi_write_jpg_to_func(jpeg_sink_write, &sink, dw, dh, 3, out_rgb,
                              PIC0_JPEG_Q)) {
    free(sink.buf);
    sink.buf = NULL;
    goto pic0_done;
  }

  *out_buf = sink.buf;
  *out_len = sink.len;
  rc = 1;
  sink.buf = NULL; /* transferred to caller */

pic0_done:
  free(file_buf);
  if (rgba) stbi_image_free(rgba);
  free(out_rgb);
  free(sink.buf);
  return rc;
}


static enum MHD_Result
appdb_pic0_request(struct MHD_Connection *conn) {
  const char *id = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                               "id");
  unsigned char *jpeg = NULL;
  size_t jpeg_len = 0;
  enum MHD_Result ret;
  struct MHD_Response *resp;

  if(!id || !is_safe_title_id(id)) {
    return serve_buffer(conn, MHD_HTTP_BAD_REQUEST, "text/plain",
                        "bad id", 6, 0);
  }

  if(!build_pic0_jpeg(id, &jpeg, &jpeg_len)) {
    return serve_buffer(conn, MHD_HTTP_NOT_FOUND, "text/plain",
                        "no pic0", 7, 0);
  }

  resp = MHD_create_response_from_buffer(jpeg_len, jpeg, MHD_RESPMEM_MUST_FREE);
  if(!resp) {
    free(jpeg);
    return serve_buffer(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "text/plain",
                        "alloc", 5, 0);
  }

  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "image/jpeg");
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL,
                          "public, max-age=86400");
  ret = websrv_queue_response(conn, MHD_HTTP_OK, resp);
  MHD_destroy_response(resp);
  return ret;
}


/* Decode common HTML entities in-place. Just enough to surface a
   readable title; we don't need a full HTML parser. */
static void
html_unescape(char *s) {
  static const struct { const char *e; char r; } repl[] = {
    {"&amp;",  '&'}, {"&#39;", '\''}, {"&apos;", '\''},
    {"&quot;", '"'}, {"&lt;",  '<'},  {"&gt;",   '>'},
  };
  char *src = s, *dst = s;
  while(*src) {
    int matched = 0;
    for(size_t i = 0; i < sizeof(repl)/sizeof(repl[0]); i++) {
      size_t n = strlen(repl[i].e);
      if(strncmp(src, repl[i].e, n) == 0) {
        *dst++ = repl[i].r;
        src += n;
        matched = 1;
        break;
      }
    }
    if(!matched) *dst++ = *src++;
  }
  *dst = 0;
}


static int
extract_patch_site_name(const char *html, size_t html_len,
                        const char *title_id,
                        char *out, size_t out_size) {
  const char *p = (const char*)memchr(html, '<', html_len);
  while(p) {
    if(p + 7 <= html + html_len &&
       (strncasecmp(p, "<title>", 7) == 0)) {
      p += 7;
      const char *end = strstr(p, "</title>");
      if(!end || end > html + html_len) return -1;
      /* Skip any leading title-id prefix + ":". */
      const char *colon = (const char*)memchr(p, ':', end - p);
      const char *name  = colon ? colon + 1 : p;
      /* Skip a single leading space. */
      if(name < end && *name == ' ') name++;
      /* Cut at the " | " separator if present. */
      const char *bar = NULL;
      for(const char *q = name; q + 3 <= end; q++) {
        if(q[0] == ' ' && q[1] == '|' && q[2] == ' ') { bar = q; break; }
      }
      const char *finish = bar ? bar : end;
      /* "Oh no... 404 :(" → no real title. */
      if(strstr(name, "Oh no") || strstr(name, "404")) return -1;
      size_t len = (size_t)(finish - name);
      if(len == 0 || len >= out_size) return -1;
      memcpy(out, name, len);
      out[len] = 0;
      html_unescape(out);
      /* Trim trailing whitespace. */
      size_t n = strlen(out);
      while(n > 0 && (out[n-1] == ' ' || out[n-1] == '\t' ||
                      out[n-1] == '\r' || out[n-1] == '\n')) {
        out[--n] = 0;
      }
      return *out ? 0 : -1;
    }
    p++;
    if(p >= html + html_len) break;
    p = (const char*)memchr(p, '<', (html + html_len) - p);
  }
  (void)title_id;
  return -1;
}


static enum MHD_Result
appdb_lookup_request(struct MHD_Connection *conn) {
  const char *id = MHD_lookup_connection_value(conn,
                          MHD_GET_ARGUMENT_KIND, "id");
  if(!id || !is_title_id(id)) {
    return serve_buffer(conn, MHD_HTTP_BAD_REQUEST, "application/json",
                        "{\"ok\":false,\"error\":\"bad id\"}", 28, 0);
  }

  char idu[16];
  for(int i = 0; i < 9; i++) {
    char c = id[i];
    if(c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    idu[i] = c;
  }
  idu[9] = 0;

  /* On-disk cache. */
  mkdir("/data", 0755);
  mkdir("/data/elf-arsenal", 0755);
  mkdir("/data/elf-arsenal/appdb-cache", 0755);

  char cache_path[160];
  snprintf(cache_path, sizeof(cache_path),
           "/data/elf-arsenal/appdb-cache/%s.txt", idu);

  char name[256] = {0};
  const char *source = "cache";
  struct stat st;
  if(stat(cache_path, &st) == 0 && st.st_size > 0 && st.st_size < (off_t)sizeof(name)) {
    int fd = open(cache_path, O_RDONLY);
    if(fd >= 0) {
      ssize_t n = read(fd, name, sizeof(name) - 1);
      close(fd);
      if(n > 0) name[n] = 0;
      /* Trim trailing newline. */
      size_t L = strlen(name);
      while(L > 0 && (name[L-1] == '\n' || name[L-1] == '\r')) name[--L] = 0;
    }
  }

  /* Fetch live if no cache hit. */
  if(!name[0]) {
    char url[160];
    if(idu[0] == 'P') {
      snprintf(url, sizeof(url), "https://prosperopatches.com/%s", idu);
      source = "prosperopatches.com";
    } else {
      snprintf(url, sizeof(url), "https://orbispatches.com/%s", idu);
      source = "orbispatches.com";
    }
    size_t blen = 0;
    uint8_t *body = http_get(url, &blen);
    if(body && blen > 0) {
      extract_patch_site_name((const char*)body, blen, idu,
                              name, sizeof(name));
    }
    free(body);

    /* Cross-site fallback (some PS5 titles still register only on
       orbispatches if they're cross-gen). */
    if(!name[0]) {
      const char *alt = (idu[0] == 'P') ? "https://orbispatches.com/"
                                        : "https://prosperopatches.com/";
      snprintf(url, sizeof(url), "%s%s", alt, idu);
      blen = 0;
      body = http_get(url, &blen);
      if(body && blen > 0) {
        if(extract_patch_site_name((const char*)body, blen, idu,
                                   name, sizeof(name)) == 0) {
          source = (idu[0] == 'P') ? "orbispatches.com"
                                   : "prosperopatches.com";
        }
      }
      free(body);
    }

    if(name[0]) {
      int fd = open(cache_path, O_WRONLY|O_CREAT|O_TRUNC, 0644);
      if(fd >= 0) {
        write(fd, name, strlen(name));
        close(fd);
      }
    }
  }

  char body[768];
  int blen;
  if(name[0]) {
    /* JSON-escape the bare minimum (\, "). */
    char esc[512];
    size_t o = 0;
    for(size_t i = 0; name[i] && o + 2 < sizeof(esc); i++) {
      char c = name[i];
      if(c == '\\' || c == '"') esc[o++] = '\\';
      esc[o++] = c;
    }
    esc[o] = 0;
    blen = snprintf(body, sizeof(body),
        "{\"ok\":true,\"id\":\"%s\",\"name\":\"%s\",\"source\":\"%s\"}",
        idu, esc, source);
  } else {
    blen = snprintf(body, sizeof(body),
        "{\"ok\":false,\"id\":\"%s\",\"error\":\"no match on patch sites\"}",
        idu);
  }
  return serve_buffer(conn, MHD_HTTP_OK, "application/json",
                      body, (size_t)blen, 0);
}


enum MHD_Result
appdb_request(struct MHD_Connection *conn, const char *url) {
  if(!strcmp(url, "/appdb") || !strcmp(url, "/appdb/")) {
    return appdb_list_request(conn);
  }
  if(!strcmp(url, "/appdb/icon")) {
    return appdb_icon_request(conn);
  }
  if(!strcmp(url, "/appdb/pic0")) {
    return appdb_pic0_request(conn);
  }
  if(!strcmp(url, "/appdb/lookup")) {
    return appdb_lookup_request(conn);
  }
  return serve_buffer(conn, MHD_HTTP_NOT_FOUND, "text/plain",
                      "not found", 9, 0);
}
