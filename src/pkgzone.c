/* Copyright (C) 2026 soniciso

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <microhttpd.h>

#include "pkgzone.h"
#include "websrv.h"
#include "ps5/http.h"
#include "third_party/stb_image.h"
#include "third_party/stb_image_write.h"

#define PKGZONE_LIST_URL_FMT  "https://pkg-zone.com/?page=%d"
#define PKGZONE_COVER_URL_FMT "https://pkg-zone.com/images/%s/cover.png"
#define PKGZONE_CACHE_DIR     "/data/elf-arsenal/cache"
#define PKGZONE_LIST_CACHE    PKGZONE_CACHE_DIR "/pkgzone-list.json"
#define PKGZONE_COVER_DIR     PKGZONE_CACHE_DIR "/pkgzone-covers"
#define PKGZONE_LIST_TTL_SEC  (7 * 24 * 60 * 60)
#define PKGZONE_MAX_PAGES     20

/* Same downscale/JPEG params as /appdb/icon — gives us PS5-friendly,
   ~30 KB covers instead of the 200 KB+ PNGs pkg-zone serves directly. */
#define COVER_MAX_DIM         256
#define COVER_JPEG_Q          80


static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char           *g_json     = NULL;
static size_t          g_json_len = 0;
static time_t          g_fetched  = 0;
static char            g_error[256] = {0};



static enum MHD_Result
serve_buf(struct MHD_Connection *conn, unsigned int status,
          const char *mime, void *data, size_t size, int free_after) {
  enum MHD_Result ret = MHD_NO;
  struct MHD_Response *resp;
  enum MHD_ResponseMemoryMode mode = free_after ? MHD_RESPMEM_MUST_FREE
                                                : MHD_RESPMEM_PERSISTENT;
  if((resp = MHD_create_response_from_buffer(size, data, mode))) {
    if(mime)
      MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, mime);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-cache");
    ret = websrv_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
  } else if(free_after) {
    free(data);
  }
  return ret;
}

static enum MHD_Result
serve_error(struct MHD_Connection *conn, unsigned int status,
            const char *msg) {
  char body[512];
  int n = snprintf(body, sizeof(body),
                   "{\"ok\":false,\"error\":\"%s\"}", msg ? msg : "");
  char *dup = strdup(body);
  if(!dup) return MHD_NO;
  return serve_buf(conn, status, "application/json", dup,
                   (size_t)n, 1);
}



typedef struct {
  char  *buf;
  size_t len;
  size_t cap;
} sb_t;

static int sb_grow(sb_t *s, size_t add) {
  if(s->len + add + 1 <= s->cap) return 0;
  size_t cap = s->cap ? s->cap : 1024;
  while(cap < s->len + add + 1) cap *= 2;
  char *p = realloc(s->buf, cap);
  if(!p) return -1;
  s->buf = p;
  s->cap = cap;
  return 0;
}
static int sb_putc(sb_t *s, char c) {
  if(sb_grow(s, 1) < 0) return -1;
  s->buf[s->len++] = c;
  s->buf[s->len]   = 0;
  return 0;
}
static int sb_puts(sb_t *s, const char *str) {
  size_t n = strlen(str);
  if(sb_grow(s, n) < 0) return -1;
  memcpy(s->buf + s->len, str, n);
  s->len += n;
  s->buf[s->len] = 0;
  return 0;
}
static int sb_put_json_string(sb_t *s, const char *str) {
  if(sb_putc(s, '"') < 0) return -1;
  for(const unsigned char *p = (const unsigned char *)str; *p; p++) {
    switch(*p) {
      case '"':  if(sb_puts(s, "\\\"") < 0) return -1; break;
      case '\\': if(sb_puts(s, "\\\\") < 0) return -1; break;
      case '\n': if(sb_puts(s, "\\n")  < 0) return -1; break;
      case '\r': if(sb_puts(s, "\\r")  < 0) return -1; break;
      case '\t': if(sb_puts(s, "\\t")  < 0) return -1; break;
      default:
        if(*p < 0x20) {
          char esc[8];
          snprintf(esc, sizeof(esc), "\\u%04x", *p);
          if(sb_puts(s, esc) < 0) return -1;
        } else {
          if(sb_putc(s, (char)*p) < 0) return -1;
        }
    }
  }
  return sb_putc(s, '"');
}



static const char *
find_in(const char *from, const char *limit, const char *needle) {
  size_t n = strlen(needle);
  if(!from || from >= limit || !needle || !*needle) return NULL;
  for(const char *p = from; p + n <= limit; p++) {
    if(*p == *needle && memcmp(p, needle, n) == 0) return p;
  }
  return NULL;
}

static void
extract_trim(const char *from, const char *end, char *out, size_t out_sz) {
  while(from < end && isspace((unsigned char)*from)) from++;
  while(end > from && isspace((unsigned char)end[-1])) end--;
  size_t w = 0;
  for(const char *p = from; p < end && w + 1 < out_sz; p++) {
    if(*p == '&') {
      if(end - p >= 5 && memcmp(p, "&amp;", 5) == 0)  { out[w++] = '&';  p += 4; continue; }
      if(end - p >= 4 && memcmp(p, "&lt;", 4) == 0)   { out[w++] = '<';  p += 3; continue; }
      if(end - p >= 4 && memcmp(p, "&gt;", 4) == 0)   { out[w++] = '>';  p += 3; continue; }
      if(end - p >= 6 && memcmp(p, "&quot;", 6) == 0) { out[w++] = '"';  p += 5; continue; }
      if(end - p >= 6 && memcmp(p, "&#039;", 6) == 0) { out[w++] = '\''; p += 5; continue; }
      if(end - p >= 5 && memcmp(p, "&#39;", 5) == 0)  { out[w++] = '\''; p += 4; continue; }
    }
    out[w++] = *p;
  }
  out[w] = 0;
}



/* Emits one JSON object per recognised card. Returns 1 on emit. */
static int
parse_card(const char *block_start, const char *block_end, sb_t *sb,
           int first) {
  const char *did = find_in(block_start, block_end, "/details/");
  if(!did) return 0;
  did += sizeof("/details/") - 1;
  char id[64] = {0};
  size_t w = 0;
  while(did < block_end && w + 1 < sizeof(id)
        && (isalnum((unsigned char)*did) || *did == '_')) {
    id[w++] = *did++;
  }
  if(!w) return 0;

  int supports_ps5 = find_in(block_start, block_end, "Supports PS5") != NULL;

  char version[64] = "";
  const char *v = find_in(block_start, block_end, "class=\"number ");
  if(v) {
    const char *open = find_in(v, block_end, ">");
    if(open) {
      const char *close = find_in(open + 1, block_end, "<");
      if(close) extract_trim(open + 1, close, version, sizeof(version));
    }
  }

  char title[256] = "";
  const char *t = find_in(block_start, block_end, "class=\"title font-bold\"");
  if(t) {
    const char *open = find_in(t, block_end, ">");
    if(open) {
      const char *close = find_in(open + 1, block_end, "</div>");
      if(close) extract_trim(open + 1, close, title, sizeof(title));
    }
  }
  if(!title[0]) snprintf(title, sizeof(title), "%s", id);

  char author[128] = "";
  const char *search_from = t ? t : block_start;
  const char *a = find_in(search_from, block_end, "text-gray-300\">");
  if(a) {
    a += sizeof("text-gray-300\">") - 1;
    const char *close = find_in(a, block_end, "</div>");
    if(close) extract_trim(a, close, author, sizeof(author));
  }

  char haystack[640];
  snprintf(haystack, sizeof(haystack), "%s %s %s%s",
           title, author, id, supports_ps5 ? " ps5" : " ps4");
  for(char *p = haystack; *p; p++) *p = (char)tolower((unsigned char)*p);

  /* Cover proxy lives on Elf Arsenal so the PS5 in-app browser doesn't
     have to reach the external HTTPS origin. */
  char cover[128];
  snprintf(cover, sizeof(cover), "/api/pkgzone/cover?id=%s", id);

  /* Build BOTH download URLs. The frontend will pick PS5 when supported,
     and warn-with-confirm for PS4-only items. */
  char download_ps5[160], download_ps4[160], details[160];
  snprintf(download_ps5, sizeof(download_ps5),
           "https://pkg-zone.com/download/ps5/%s/latest", id);
  snprintf(download_ps4, sizeof(download_ps4),
           "https://pkg-zone.com/download/ps4/%s/latest", id);
  snprintf(details, sizeof(details),
           "https://pkg-zone.com/details/%s", id);

  if(!first) sb_putc(sb, ',');
  sb_puts(sb, "\n{\"id\":");            sb_put_json_string(sb, id);
  sb_puts(sb, ",\"title\":");           sb_put_json_string(sb, title);
  sb_puts(sb, ",\"version\":");         sb_put_json_string(sb, version);
  sb_puts(sb, ",\"author\":");          sb_put_json_string(sb, author);
  sb_puts(sb, ",\"supportsPs5\":");     sb_puts(sb, supports_ps5 ? "true" : "false");
  sb_puts(sb, ",\"coverUrl\":");        sb_put_json_string(sb, cover);
  sb_puts(sb, ",\"downloadPs5Url\":");  sb_put_json_string(sb, download_ps5);
  sb_puts(sb, ",\"downloadPs4Url\":");  sb_put_json_string(sb, download_ps4);
  sb_puts(sb, ",\"detailsUrl\":");      sb_put_json_string(sb, details);
  sb_puts(sb, ",\"haystack\":");        sb_put_json_string(sb, haystack);
  sb_puts(sb, "}");
  return 1;
}

/* Returns number of cards parsed from this page's HTML. */
static int
parse_one_page(const char *html, size_t html_len, sb_t *sb, int *first_io) {
  int parsed = 0;
  const char *end = html + html_len;
  const char *cur = html;
  while(cur < end) {
    const char *card = find_in(cur, end, "<article class=\"pkg ");
    if(!card) break;
    const char *next = find_in(card + 1, end, "<article class=\"pkg ");
    const char *limit = next ? next : end;
    if(parse_card(card, limit, sb, *first_io)) {
      *first_io = 0;
      parsed++;
    }
    cur = limit;
  }
  return parsed;
}



static int
mkdirs_for(const char *path) {
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for(char *p = tmp + 1; *p; p++) {
    if(*p == '/') {
      *p = 0;
      if(mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
      *p = '/';
    }
  }
  return 0;
}

static int
write_file_atomic(const char *path, const void *data, size_t len) {
  if(mkdirs_for(path) < 0) return -1;
  char tmp[600];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *f = fopen(tmp, "wb");
  if(!f) return -1;
  size_t w = fwrite(data, 1, len, f);
  fclose(f);
  if(w != len) { unlink(tmp); return -1; }
  if(rename(tmp, path) < 0) { unlink(tmp); return -1; }
  return 0;
}

static char *
read_file(const char *path, size_t *out_len, time_t *out_mtime) {
  struct stat st;
  if(stat(path, &st) < 0) return NULL;
  FILE *f = fopen(path, "rb");
  if(!f) return NULL;
  char *buf = malloc((size_t)st.st_size + 1);
  if(!buf) { fclose(f); return NULL; }
  size_t r = fread(buf, 1, (size_t)st.st_size, f);
  fclose(f);
  if(r != (size_t)st.st_size) { free(buf); return NULL; }
  buf[st.st_size] = 0;
  if(out_len)   *out_len   = (size_t)st.st_size;
  if(out_mtime) *out_mtime = st.st_mtime;
  return buf;
}

/* Title-id-style ID validator: alnum + underscore only, max 40 chars.
   Keeps a malicious "?id=../../etc/passwd" out of the cache path. */
static int
is_safe_id(const char *id) {
  if(!id || !*id) return 0;
  size_t n = strlen(id);
  if(n > 40) return 0;
  for(size_t i = 0; i < n; i++) {
    char c = id[i];
    if(!isalnum((unsigned char)c) && c != '_') return 0;
  }
  return 1;
}



/* Builds {ok, fetchedAt, items:[…]} by walking pkg-zone pages until
   one returns 0 cards. */
static char *
build_full_listing(size_t *out_len) {
  sb_t sb = {0};
  if(sb_puts(&sb, "{\"ok\":true,\"fetchedAt\":") < 0) goto fail;
  char ts[32];
  snprintf(ts, sizeof(ts), "%lld", (long long)time(NULL));
  if(sb_puts(&sb, ts) < 0) goto fail;
  if(sb_puts(&sb, ",\"items\":[") < 0) goto fail;

  int first = 1;
  int total = 0;
  for(int page = 1; page <= PKGZONE_MAX_PAGES; page++) {
    char url[128];
    snprintf(url, sizeof(url), PKGZONE_LIST_URL_FMT, page);
    size_t html_len = 0;
    uint8_t *html = http_get(url, &html_len);
    if(!html || !html_len) {
      free(html);
      if(total == 0) goto fail;     /* couldn't even get page 1 */
      break;                        /* later pages just stop the loop */
    }
    int n = parse_one_page((const char *)html, html_len, &sb, &first);
    free(html);
    total += n;
    if(n == 0) break;
  }
  if(sb_puts(&sb, "]}") < 0) goto fail;
  *out_len = sb.len;
  return sb.buf;

fail:
  free(sb.buf);
  return NULL;
}

static int
fetch_and_cache_list(void) {
  size_t len = 0;
  char *json = build_full_listing(&len);
  if(!json) {
    pthread_mutex_lock(&g_lock);
    snprintf(g_error, sizeof(g_error),
             "pkg-zone.com fetch failed (network/SSL or markup changed).");
    pthread_mutex_unlock(&g_lock);
    return -1;
  }
  pthread_mutex_lock(&g_lock);
  free(g_json);
  g_json     = json;
  g_json_len = len;
  g_fetched  = time(NULL);
  g_error[0] = 0;
  write_file_atomic(PKGZONE_LIST_CACHE, g_json, g_json_len);
  pthread_mutex_unlock(&g_lock);
  return 0;
}

static void
prime_list_from_disk_if_needed(void) {
  pthread_mutex_lock(&g_lock);
  int need = (g_json == NULL);
  pthread_mutex_unlock(&g_lock);
  if(!need) return;

  size_t len = 0;
  time_t mtime = 0;
  char *buf = read_file(PKGZONE_LIST_CACHE, &len, &mtime);
  if(!buf) return;
  pthread_mutex_lock(&g_lock);
  if(g_json == NULL) {
    g_json     = buf;
    g_json_len = len;
    g_fetched  = mtime;
    buf = NULL;
  }
  pthread_mutex_unlock(&g_lock);
  free(buf);
}



typedef struct {
  unsigned char *buf;
  size_t        len;
  size_t        cap;
} jpeg_sink_t;

static void
jpeg_sink_write(void *cls, void *data, int size) {
  jpeg_sink_t *s = cls;
  if(s->len + (size_t)size > s->cap) {
    s->cap = (s->len + (size_t)size) * 2;
    s->buf = realloc(s->buf, s->cap);
  }
  memcpy(s->buf + s->len, data, (size_t)size);
  s->len += (size_t)size;
}

/* Fetch pkg-zone cover for `id`, decode, downscale, JPEG-encode.
   Writes to PKGZONE_COVER_DIR/<id>.jpg. Returns 0 on success. */
static int
build_and_cache_cover(const char *id, const char *out_path) {
  char src_url[256];
  snprintf(src_url, sizeof(src_url), PKGZONE_COVER_URL_FMT, id);

  size_t png_len = 0;
  uint8_t *png = http_get(src_url, &png_len);
  if(!png || !png_len) { free(png); return -1; }

  int w = 0, h = 0, c = 0;
  unsigned char *rgba = stbi_load_from_memory(png, (int)png_len, &w, &h, &c, 4);
  free(png);
  if(!rgba || w <= 0 || h <= 0) {
    if(rgba) stbi_image_free(rgba);
    return -1;
  }

  int dw = w, dh = h;
  if(w > COVER_MAX_DIM || h > COVER_MAX_DIM) {
    if(w >= h) {
      dw = COVER_MAX_DIM;
      dh = (h * COVER_MAX_DIM + w/2) / w;
    } else {
      dh = COVER_MAX_DIM;
      dw = (w * COVER_MAX_DIM + h/2) / h;
    }
    if(dw < 1) dw = 1;
    if(dh < 1) dh = 1;
  }

  unsigned char *rgb = malloc((size_t)dw * (size_t)dh * 3);
  if(!rgb) { stbi_image_free(rgba); return -1; }

  if(dw == w && dh == h) {
    for(int i = 0; i < w*h; i++) {
      rgb[i*3+0] = rgba[i*4+0];
      rgb[i*3+1] = rgba[i*4+1];
      rgb[i*3+2] = rgba[i*4+2];
    }
  } else {
    /* Average-box downscale — same approach the appdb icon path uses. */
    for(int y = 0; y < dh; y++) {
      int sy0 = (y    * h) / dh;
      int sy1 = ((y+1)* h + dh - 1) / dh;
      if(sy1 > h) sy1 = h;
      if(sy1 <= sy0) sy1 = sy0 + 1;
      for(int x = 0; x < dw; x++) {
        int sx0 = (x    * w) / dw;
        int sx1 = ((x+1)* w + dw - 1) / dw;
        if(sx1 > w) sx1 = w;
        if(sx1 <= sx0) sx1 = sx0 + 1;
        unsigned r = 0, g = 0, b = 0, n = 0;
        for(int yy = sy0; yy < sy1; yy++) {
          for(int xx = sx0; xx < sx1; xx++) {
            unsigned char *p = rgba + (yy*w + xx)*4;
            r += p[0]; g += p[1]; b += p[2]; n++;
          }
        }
        if(!n) n = 1;
        unsigned char *o = rgb + (y*dw + x)*3;
        o[0] = (unsigned char)(r / n);
        o[1] = (unsigned char)(g / n);
        o[2] = (unsigned char)(b / n);
      }
    }
  }
  stbi_image_free(rgba);

  jpeg_sink_t sink = {0};
  sink.cap = 32 * 1024;
  sink.buf = malloc(sink.cap);
  if(!sink.buf) { free(rgb); return -1; }

  int ok = stbi_write_jpg_to_func(jpeg_sink_write, &sink, dw, dh, 3, rgb,
                                  COVER_JPEG_Q);
  free(rgb);
  if(!ok) { free(sink.buf); return -1; }

  int rc = write_file_atomic(out_path, sink.buf, sink.len);
  free(sink.buf);
  return rc;
}

static enum MHD_Result
cover_request(struct MHD_Connection *conn) {
  const char *id = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                               "id");
  if(!is_safe_id(id)) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "bad id");
  }

  char path[256];
  snprintf(path, sizeof(path), "%s/%s.jpg", PKGZONE_COVER_DIR, id);

  struct stat st;
  if(stat(path, &st) != 0) {
    if(build_and_cache_cover(id, path) != 0) {
      return serve_error(conn, MHD_HTTP_BAD_GATEWAY,
                         "cover fetch/encode failed");
    }
    if(stat(path, &st) != 0) {
      return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                         "cover stat failed after write");
    }
  }

  /* Stream the JPEG out from disk. */
  int fd = open(path, O_RDONLY);
  if(fd < 0) {
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                       "cover open failed");
  }
  void *buf = malloc((size_t)st.st_size);
  if(!buf) { close(fd); return serve_error(conn, 500, "alloc"); }
  ssize_t r = read(fd, buf, (size_t)st.st_size);
  close(fd);
  if(r != st.st_size) { free(buf); return serve_error(conn, 500, "read"); }

  struct MHD_Response *resp = MHD_create_response_from_buffer(
      (size_t)st.st_size, buf, MHD_RESPMEM_MUST_FREE);
  if(!resp) { free(buf); return MHD_NO; }
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "image/jpeg");
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL,
                          "public, max-age=86400");
  enum MHD_Result ret = websrv_queue_response(conn, MHD_HTTP_OK, resp);
  MHD_destroy_response(resp);
  return ret;
}



static enum MHD_Result
list_request(struct MHD_Connection *conn) {
  prime_list_from_disk_if_needed();

  time_t now = time(NULL);
  int need_fetch;
  pthread_mutex_lock(&g_lock);
  need_fetch = (g_json == NULL) ||
               (now - g_fetched >= PKGZONE_LIST_TTL_SEC);
  pthread_mutex_unlock(&g_lock);

  if(need_fetch) fetch_and_cache_list();

  pthread_mutex_lock(&g_lock);
  if(!g_json) {
    char err[256];
    snprintf(err, sizeof(err),
             "pkg-zone.com fetch failed and no cached listing is available "
             "yet. Detail: %s",
             g_error[0] ? g_error : "(no error)");
    pthread_mutex_unlock(&g_lock);
    return serve_error(conn, MHD_HTTP_BAD_GATEWAY, err);
  }
  char  *dup = malloc(g_json_len + 1);
  size_t dup_len = g_json_len;
  if(dup) {
    memcpy(dup, g_json, g_json_len);
    dup[g_json_len] = 0;
  }
  pthread_mutex_unlock(&g_lock);
  if(!dup) return serve_error(conn, 500, "out of memory");
  return serve_buf(conn, MHD_HTTP_OK, "application/json",
                   dup, dup_len, 1);
}

static enum MHD_Result
refresh_request(struct MHD_Connection *conn) {
  int rc = fetch_and_cache_list();
  if(rc != 0) {
    char err[256];
    pthread_mutex_lock(&g_lock);
    snprintf(err, sizeof(err), "%s",
             g_error[0] ? g_error : "fetch failed");
    pthread_mutex_unlock(&g_lock);
    return serve_error(conn, MHD_HTTP_BAD_GATEWAY, err);
  }
  return list_request(conn);
}


enum MHD_Result
pkgzone_request(struct MHD_Connection *conn, const char *url) {
  if(!strcmp(url, "/api/pkgzone/list")) {
    return list_request(conn);
  }
  if(!strcmp(url, "/api/pkgzone/refresh")) {
    return refresh_request(conn);
  }
  if(!strcmp(url, "/api/pkgzone/cover")) {
    return cover_request(conn);
  }
  return serve_error(conn, MHD_HTTP_NOT_FOUND, "no such endpoint");
}
