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


#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <microhttpd.h>

#include "tmdb.h"
#include "websrv.h"
#include "ps5/http.h"
#include "third_party/cJSON.h"


#define CACHE_DIR     "/data/elf-arsenal/tmdb"
#define CACHE_TTL_S   (30 * 24 * 60 * 60)
#define STORE_BASE    "https://store.playstation.com/en-us/product/"

#define API_PREFIX    "/api/tmdb/"
#define API_PREFIX_LN (sizeof(API_PREFIX) - 1)


static const char *const REGION_PREFIXES[] = {
    "IP9100", "UP9000", "UP0006", "UP0002", "UP0700", "UP0177", "UP0082",
    "UP4040", "UP4108", "UP4415", "EP9000", "EP0006", "EP0002", "EP0700",
    "EP0177", "EP4108", "EP4415", "EP1018", "HP9000", "JP9000", "JP9001",
    "UB1019", "UB0335", "UB1229", "UB0006",
    NULL,
};

static const char *const TRAILING_LABELS[] = {
    "PREINMASTER00000",
    "0000000000000000",
    NULL,
};



static enum MHD_Result
serve_buf(struct MHD_Connection *conn, unsigned int status,
          const char *mime, void *data, size_t size, int free_after) {
  enum MHD_Result ret = MHD_NO;
  enum MHD_ResponseMemoryMode mode = free_after ? MHD_RESPMEM_MUST_FREE
                                                : MHD_RESPMEM_PERSISTENT;
  struct MHD_Response *resp = MHD_create_response_from_buffer(size, data, mode);
  if(resp) {
    if(mime)
      MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, mime);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL,
                            "public, max-age=86400");
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
  char body[256];
  int n = snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}",
                   msg ? msg : "unknown");
  char *dup = strdup(body);
  if(!dup) return MHD_NO;
  return serve_buf(conn, status, "application/json", dup, (size_t)n, 1);
}



static int
is_title_id(const char *s) {
  /* AAAANNNNN_NN — four letters (or digits in some entry-point IDs),
     five digits, underscore, two digits. */
  if(!s || strlen(s) != 12) return 0;
  for(int i = 0; i < 4; i++) if(!isupper((unsigned char)s[i])) return 0;
  for(int i = 4; i < 9; i++) if(!isdigit((unsigned char)s[i])) return 0;
  if(s[9] != '_') return 0;
  if(!isdigit((unsigned char)s[10]) || !isdigit((unsigned char)s[11])) return 0;
  return 1;
}

static int
is_content_id(const char *s) {
  /* XXXXXX-TITLEID_00-YYYYYYYYYYYYYYYY — 36 chars total. */
  if(!s || strlen(s) != 36) return 0;
  if(s[6] != '-' || s[19] != '-') return 0;
  char tid[13];
  memcpy(tid, s + 7, 12); tid[12] = '\0';
  if(!is_title_id(tid)) return 0;
  return 1;
}

static void
title_from_content(const char *content_id, char *out_title) {
  memcpy(out_title, content_id + 7, 12);
  out_title[12] = '\0';
}



static const char *
mem_find(const char *hay, size_t hay_len, const char *needle) {
  size_t nl = strlen(needle);
  if(nl == 0 || hay_len < nl) return NULL;
  for(size_t i = 0; i + nl <= hay_len; i++) {
    if(hay[i] == needle[0] && !memcmp(hay + i, needle, nl)) {
      return hay + i;
    }
  }
  return NULL;
}



static int
page_has_product(const char *html, size_t html_len,
                 const char *content_id) {
  char needle[64];
  snprintf(needle, sizeof(needle), "Product:%s", content_id);
  return mem_find(html, html_len, needle) != NULL;
}

static char *
extract_jsonld(const char *html, size_t html_len) {
  static const char OPEN[] =
      "<script id=\"mfe-jsonld-tags\" type=\"application/ld+json\">";
  static const char CLOSE[] = "</script>";
  const char *p = mem_find(html, html_len, OPEN);
  if(!p) return NULL;
  p += sizeof(OPEN) - 1;
  size_t remaining = html_len - (size_t)(p - html);
  const char *q = mem_find(p, remaining, CLOSE);
  if(!q) return NULL;
  size_t len = (size_t)(q - p);
  char *out = malloc(len + 1);
  if(!out) return NULL;
  memcpy(out, p, len);
  out[len] = '\0';
  return out;
}



static char *
fetch_jsonld_for(const char *content_id) {
  char url[256];
  snprintf(url, sizeof(url), STORE_BASE "%s", content_id);

  size_t html_len = 0;
  uint8_t *html = http_get(url, &html_len);
  if(!html || html_len == 0) {
    if(html) free(html);
    return NULL;
  }
  if(!page_has_product((const char *)html, html_len, content_id)) {
    free(html);
    return NULL;
  }
  char *jsonld = extract_jsonld((const char *)html, html_len);
  free(html);
  return jsonld;
}

static char *
resolve_title_id(const char *title_id, char *out_content_id,
                 size_t out_content_size) {
  char cid[40];
  for(int i = 0; REGION_PREFIXES[i]; i++) {
    for(int j = 0; TRAILING_LABELS[j]; j++) {
      snprintf(cid, sizeof(cid), "%s-%s-%s",
               REGION_PREFIXES[i], title_id, TRAILING_LABELS[j]);
      char *jsonld = fetch_jsonld_for(cid);
      if(jsonld) {
        strncpy(out_content_id, cid, out_content_size - 1);
        out_content_id[out_content_size - 1] = '\0';
        return jsonld;
      }
    }
  }
  return NULL;
}


/* -------- TMDB-shape envelope --------------------------------------- */

static char *
build_tmdb_json(char *jsonld, const char *content_id, size_t *out_len) {
  cJSON *src = cJSON_Parse(jsonld);
  free(jsonld);
  if(!src) return NULL;

  cJSON *root = cJSON_CreateObject();
  if(!root) { cJSON_Delete(src); return NULL; }

  char tid[13];
  title_from_content(content_id, tid);
  cJSON_AddStringToObject(root, "npTitleId", tid);
  cJSON_AddStringToObject(root, "contentId", content_id);

  cJSON *names = cJSON_AddArrayToObject(root, "names");
  cJSON *name = cJSON_GetObjectItem(src, "name");
  if(cJSON_IsString(name) && name->valuestring) {
    cJSON *one = cJSON_CreateObject();
    cJSON_AddStringToObject(one, "name", name->valuestring);
    cJSON_AddItemToArray(names, one);
  }

  cJSON *cat = cJSON_GetObjectItem(src, "category");
  if(cJSON_IsString(cat) && cat->valuestring)
    cJSON_AddStringToObject(root, "category", cat->valuestring);

  cJSON *desc = cJSON_GetObjectItem(src, "description");
  if(cJSON_IsString(desc) && desc->valuestring)
    cJSON_AddStringToObject(root, "description", desc->valuestring);

  cJSON *img = cJSON_GetObjectItem(src, "image");
  if(cJSON_IsString(img) && img->valuestring)
    cJSON_AddStringToObject(root, "icon", img->valuestring);

  cJSON_Delete(src);

  char *out = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if(!out) return NULL;
  *out_len = strlen(out);
  return out;
}



static void
ensure_cache_dir(void) {
  mkdir("/data/elf-arsenal", 0755);
  mkdir(CACHE_DIR, 0755);
}

static char *
cache_load(const char *title_id, size_t *out_len, int allow_stale) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), CACHE_DIR "/%s.json", title_id);
  struct stat st;
  if(stat(path, &st) != 0) return NULL;
  if(!allow_stale && time(NULL) - st.st_mtime > CACHE_TTL_S) return NULL;
  FILE *f = fopen(path, "rb");
  if(!f) return NULL;
  char *buf = malloc((size_t)st.st_size + 1);
  if(!buf) { fclose(f); return NULL; }
  size_t n = fread(buf, 1, (size_t)st.st_size, f);
  fclose(f);
  buf[n] = '\0';
  *out_len = n;
  return buf;
}

static void
cache_save(const char *title_id, const char *json, size_t len) {
  ensure_cache_dir();
  char path[PATH_MAX];
  snprintf(path, sizeof(path), CACHE_DIR "/%s.json", title_id);
  FILE *f = fopen(path, "wb");
  if(!f) return;
  fwrite(json, 1, len, f);
  fclose(f);
}



enum MHD_Result
tmdb_request(struct MHD_Connection *conn, const char *url) {
  if(strncmp(url, API_PREFIX, API_PREFIX_LN) != 0) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "bad path");
  }
  const char *key = url + API_PREFIX_LN;

  /* Copy off the slug part (strip ?refresh=1 if present). */
  char id_buf[64];
  size_t i;
  for(i = 0; i < sizeof(id_buf) - 1 && key[i] && key[i] != '?'
            && key[i] != '/'; i++) {
    id_buf[i] = key[i];
  }
  id_buf[i] = '\0';
  if(!id_buf[0]) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                       "missing title id or content id");
  }

  const char *refresh = MHD_lookup_connection_value(
      conn, MHD_GET_ARGUMENT_KIND, "refresh");
  int want_refresh = refresh && refresh[0] && refresh[0] != '0';

  char content_id[40] = {0};
  char title_id[13] = {0};

  if(is_content_id(id_buf)) {
    strncpy(content_id, id_buf, sizeof(content_id) - 1);
    title_from_content(content_id, title_id);
  } else if(is_title_id(id_buf)) {
    strncpy(title_id, id_buf, sizeof(title_id) - 1);
  } else {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                       "expected TITLEID_00 or 36-char contentId");
  }

  if(!want_refresh) {
    size_t cached_len = 0;
    char *cached = cache_load(title_id, &cached_len, 0);
    if(cached) {
      return serve_buf(conn, MHD_HTTP_OK, "application/json",
                       cached, cached_len, 1);
    }
  }

  char *jsonld = NULL;
  if(content_id[0]) {
    jsonld = fetch_jsonld_for(content_id);
  } else {
    jsonld = resolve_title_id(title_id, content_id, sizeof(content_id));
  }

  if(!jsonld) {
    size_t cached_len = 0;
    char *cached = cache_load(title_id, &cached_len, 1);
    if(cached) {
      return serve_buf(conn, MHD_HTTP_OK, "application/json",
                       cached, cached_len, 1);
    }
    return serve_error(conn, MHD_HTTP_NOT_FOUND,
                       "store lookup failed");
  }

  size_t out_len = 0;
  char *out = build_tmdb_json(jsonld, content_id, &out_len);
  if(!out) {
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                       "json build failed");
  }
  cache_save(title_id, out, out_len);
  return serve_buf(conn, MHD_HTTP_OK, "application/json",
                   out, out_len, 1);
}
