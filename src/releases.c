/* Elf Arsenal — GitHub releases proxy. See releases.h. */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <microhttpd.h>

#include "releases.h"
#include "ps5/http.h"
#include "third_party/cJSON.h"
#include "websrv.h"


static const char *ALLOWED_REPOS[] = {
  "EchoStretch/kstuff-lite",
  "drakmor/kstuff-lite",
  "drakmor/ShadowMountPlus",
  "ps5-linux/ps5-linux-loader",
  NULL,
};


#define CACHE_DIR       "/data/elf-arsenal/releases-cache"
#define MAX_CACHE_REPOS 5

typedef struct {
  char    repo[64];
  char   *json_body;   /* heap, null-terminated */
  size_t  json_len;
  time_t  fetched_ts;
} cache_t;

static cache_t          g_cache[MAX_CACHE_REPOS];
static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;


/* Map a "owner/name" repo string to a safe filesystem-friendly basename
   for the on-disk cache file. */
static void
cache_path_for(const char *repo, char *out, size_t out_size) {
  char safe[80];
  size_t i;
  for(i = 0; i < sizeof(safe) - 1 && repo[i]; i++) {
    char c = repo[i];
    safe[i] = (c == '/' || c == '\\' || c == ':') ? '_' : c;
  }
  safe[i] = '\0';
  snprintf(out, out_size, "%s/%s.json", CACHE_DIR, safe);
}


/* Slurp a file into a heap buffer. Caller frees. Returns NULL on any
   I/O failure or if the file exceeds 256 KB. */
static char*
read_file_to_heap(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "r");
  if(!f) return NULL;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if(sz <= 0 || sz > 256 * 1024) { fclose(f); return NULL; }
  char *buf = malloc((size_t)sz + 1);
  if(!buf) { fclose(f); return NULL; }
  size_t n = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[n] = '\0';
  if(out_len) *out_len = n;
  return buf;
}


/* Persist a cache entry to disk. Caller holds g_lock. */
static void
persist_cache_locked(const char *repo, const char *json, size_t json_len) {
  mkdir("/data", 0755);
  mkdir("/data/elf-arsenal", 0755);
  mkdir(CACHE_DIR, 0755);
  char path[256], tmp[280];
  cache_path_for(repo, path, sizeof(path));
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *f = fopen(tmp, "w");
  if(!f) return;
  fwrite(json, 1, json_len, f);
  fclose(f);
  rename(tmp, path);
}


static int
repo_is_allowed(const char *repo) {
  if(!repo || !*repo) return 0;
  for(int i = 0; ALLOWED_REPOS[i]; i++) {
    if(!strcmp(ALLOWED_REPOS[i], repo)) return 1;
  }
  return 0;
}


static enum MHD_Result
serve_buffer(struct MHD_Connection *conn, unsigned int status,
             const char *mime, void *data, size_t size, int free_after) {
  enum MHD_Result ret = MHD_NO;
  struct MHD_Response *resp;
  enum MHD_ResponseMemoryMode mode = free_after ? MHD_RESPMEM_MUST_FREE
                                                : MHD_RESPMEM_PERSISTENT;
  if((resp = MHD_create_response_from_buffer(size, data, mode))) {
    if(mime) MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, mime);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-cache");
    ret = websrv_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
  } else if(free_after) {
    free(data);
  }
  return ret;
}

static enum MHD_Result
serve_json(struct MHD_Connection *conn, unsigned int status, cJSON *o) {
  char *txt = cJSON_PrintUnformatted(o);
  if(!txt) return serve_buffer(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                               "application/json",
                               "{\"error\":\"alloc\"}", 17, 0);
  return serve_buffer(conn, status, "application/json", txt, strlen(txt), 1);
}

static enum MHD_Result
serve_error(struct MHD_Connection *conn, unsigned int status, const char *msg) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o,   "ok",    0);
  cJSON_AddStringToObject(o, "error", msg);
  enum MHD_Result ret = serve_json(conn, status, o);
  cJSON_Delete(o);
  return ret;
}


/* Build the slim JSON response from the raw GitHub releases array. */
static cJSON*
slim_releases(const char *raw, size_t raw_len) {
  cJSON *root = cJSON_ParseWithLength(raw, raw_len);
  if(!root || !cJSON_IsArray(root)) {
    if(root) cJSON_Delete(root);
    return NULL;
  }
  cJSON *out = cJSON_CreateArray();
  cJSON *r;
  cJSON_ArrayForEach(r, root) {
    cJSON *tag      = cJSON_GetObjectItem(r, "tag_name");
    cJSON *pub      = cJSON_GetObjectItem(r, "published_at");
    cJSON *assets   = cJSON_GetObjectItem(r, "assets");
    cJSON *draft    = cJSON_GetObjectItem(r, "draft");
    if(!cJSON_IsString(tag) || !cJSON_IsArray(assets)) continue;
    if(cJSON_IsBool(draft) && cJSON_IsTrue(draft))    continue;

    cJSON *best = NULL;
    int    zip_only = 0;
    cJSON *a;
    cJSON_ArrayForEach(a, assets) {
      cJSON *n = cJSON_GetObjectItem(a, "name");
      if(!cJSON_IsString(n)) continue;
      const char *name = n->valuestring;
      size_t nl = strlen(name);
      if(nl >= 4 && !strcasecmp(name + nl - 4, ".elf")) {
        if(!strcasecmp(name, "kstuff.elf") ||
           !strcasecmp(name, "shadowmountplus.elf") ||
           !strcasecmp(name, "shadowmount.elf")) {
          best = a;
          break;
        }
        if(!best) best = a;
      }
    }
    if(!best) {
      cJSON_ArrayForEach(a, assets) {
        cJSON *n = cJSON_GetObjectItem(a, "name");
        if(!cJSON_IsString(n)) continue;
        const char *name = n->valuestring;
        size_t nl = strlen(name);
        if(nl >= 4 && !strcasecmp(name + nl - 4, ".zip")) {
          best = a;
          zip_only = 1;
          break;
        }
      }
    }
    if(!best) continue;

    cJSON *bn = cJSON_GetObjectItem(best, "name");
    cJSON *bu = cJSON_GetObjectItem(best, "browser_download_url");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "tag",   tag->valuestring);
    if(cJSON_IsString(pub) && strlen(pub->valuestring) >= 10) {
      char date[16] = {0};
      memcpy(date, pub->valuestring, 10);
      cJSON_AddStringToObject(o, "date", date);
    }
    cJSON_AddStringToObject(o, "asset",
        cJSON_IsString(bn) ? bn->valuestring : "");
    cJSON_AddStringToObject(o, "assetUrl",
        cJSON_IsString(bu) ? bu->valuestring : "");
    cJSON_AddBoolToObject  (o, "zipOnly", zip_only);
    cJSON_AddItemToArray(out, o);
  }
  cJSON_Delete(root);
  return out;
}


/* Find or evict a cache slot for `repo`. Caller holds g_lock. */
static cache_t*
cache_slot(const char *repo) {
  /* Hit. */
  for(int i = 0; i < MAX_CACHE_REPOS; i++) {
    if(g_cache[i].repo[0] && !strcmp(g_cache[i].repo, repo))
      return &g_cache[i];
  }
  /* Empty slot. */
  for(int i = 0; i < MAX_CACHE_REPOS; i++) {
    if(!g_cache[i].repo[0]) return &g_cache[i];
  }
  /* Evict the oldest. */
  int oldest = 0;
  for(int i = 1; i < MAX_CACHE_REPOS; i++) {
    if(g_cache[i].fetched_ts < g_cache[oldest].fetched_ts) oldest = i;
  }
  cache_t *c = &g_cache[oldest];
  free(c->json_body); c->json_body = NULL; c->json_len = 0;
  c->repo[0] = 0; c->fetched_ts = 0;
  return c;
}


static char*
load_cached_locked(const char *repo, size_t *out_len) {
  cache_t *c = cache_slot(repo);
  if(c->json_body && c->json_len) {
    char *copy = malloc(c->json_len + 1);
    if(!copy) return NULL;
    memcpy(copy, c->json_body, c->json_len);
    copy[c->json_len] = '\0';
    *out_len = c->json_len;
    return copy;
  }
  /* Try disk. */
  char path[256];
  cache_path_for(repo, path, sizeof(path));
  size_t fl = 0;
  char *body = read_file_to_heap(path, &fl);
  if(!body) return NULL;
  /* Promote to in-memory. */
  c = cache_slot(repo);
  free(c->json_body);
  c->json_body = malloc(fl + 1);
  if(c->json_body) {
    memcpy(c->json_body, body, fl);
    c->json_body[fl] = '\0';
    c->json_len = fl;
    struct stat st;
    c->fetched_ts = (stat(path, &st) == 0) ? st.st_mtime : time(NULL);
    strncpy(c->repo, repo, sizeof(c->repo) - 1);
    c->repo[sizeof(c->repo) - 1] = '\0';
  }
  *out_len = fl;
  return body;
}


static char*
wrap_response(const char *releases_json, size_t releases_len,
              int from_cache, time_t fetched_ts, const char *refresh_err,
              size_t *out_len) {
  /* releases_json is itself an array string ("[...]"). Bolt it into the
     wrapper without re-parsing. */
  size_t buf_cap = releases_len + 256 +
                   (refresh_err ? strlen(refresh_err) + 32 : 0);
  char *buf = malloc(buf_cap);
  if(!buf) return NULL;
  int n = snprintf(buf, buf_cap,
      "{\"ok\":true,\"fromCache\":%s,\"fetchedTs\":%lld,\"releases\":",
      from_cache ? "true" : "false",
      (long long)fetched_ts);
  if(n < 0 || (size_t)n >= buf_cap) { free(buf); return NULL; }
  size_t off = (size_t)n;
  memcpy(buf + off, releases_json, releases_len);
  off += releases_len;
  if(refresh_err) {
    /* Minimal JSON-string escape — refresh_err comes from us, no quotes. */
    n = snprintf(buf + off, buf_cap - off, ",\"refreshError\":\"%s\"}",
                 refresh_err);
    if(n < 0 || (size_t)n >= buf_cap - off) { free(buf); return NULL; }
    off += (size_t)n;
  } else {
    if(off + 1 >= buf_cap) { free(buf); return NULL; }
    buf[off++] = '}';
  }
  buf[off] = '\0';
  *out_len = off;
  return buf;
}


static enum MHD_Result
list_request(struct MHD_Connection *conn) {
  const char *repo  = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                                  "repo");
  const char *force = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                                  "fresh");
  if(!repo || !*repo)
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                       "missing ?repo=<owner>/<name>");
  if(!repo_is_allowed(repo))
    return serve_error(conn, MHD_HTTP_FORBIDDEN,
                       "repo not whitelisted");

  int fresh = (force && (!strcmp(force, "1") || !strcasecmp(force, "true")));

  if(!fresh) {
    pthread_mutex_lock(&g_lock);
    size_t cached_len = 0;
    char *cached = load_cached_locked(repo, &cached_len);
    time_t fetched_ts = 0;
    if(cached) {
      cache_t *c = cache_slot(repo);
      fetched_ts = c->fetched_ts;
    }
    pthread_mutex_unlock(&g_lock);

    if(cached) {
      size_t wrapped_len = 0;
      char *wrapped = wrap_response(cached, cached_len, 1, fetched_ts,
                                    NULL, &wrapped_len);
      free(cached);
      if(wrapped)
        return serve_buffer(conn, MHD_HTTP_OK, "application/json",
                            wrapped, wrapped_len, 1);
      return serve_error(conn, 500, "alloc");
    }
    /* No cache yet → fall through to a fetch attempt. */
  }

  /* Fetch upstream. */
  char url[256];
  snprintf(url, sizeof(url),
           "https://api.github.com/repos/%s/releases?per_page=100", repo);
  size_t raw_len = 0;
  uint8_t *raw = http_get(url, &raw_len);

  cJSON *slim = NULL;
  if(raw && raw_len) slim = slim_releases((const char*)raw, raw_len);
  free(raw);

  if(!slim) {
    pthread_mutex_lock(&g_lock);
    size_t cached_len = 0;
    char *cached = load_cached_locked(repo, &cached_len);
    time_t fetched_ts = 0;
    if(cached) {
      cache_t *c = cache_slot(repo);
      fetched_ts = c->fetched_ts;
    }
    pthread_mutex_unlock(&g_lock);
    if(cached) {
      size_t wrapped_len = 0;
      char *wrapped = wrap_response(cached, cached_len, 1, fetched_ts,
          "GitHub fetch failed (rate-limit, network, etc.) — showing cached releases",
          &wrapped_len);
      free(cached);
      if(wrapped)
        return serve_buffer(conn, MHD_HTTP_OK, "application/json",
                            wrapped, wrapped_len, 1);
    }
    return serve_error(conn, MHD_HTTP_BAD_GATEWAY,
                       "GitHub fetch failed and no cached data is available yet");
  }

  /* Got a fresh successful fetch. */
  char *txt = cJSON_PrintUnformatted(slim);
  cJSON_Delete(slim);
  if(!txt) return serve_error(conn, 500, "alloc");
  size_t txt_len = strlen(txt);
  time_t now = time(NULL);

  /* Replace in-memory + disk cache. */
  pthread_mutex_lock(&g_lock);
  cache_t *c = cache_slot(repo);
  free(c->json_body);
  c->json_body = malloc(txt_len + 1);
  if(c->json_body) {
    memcpy(c->json_body, txt, txt_len);
    c->json_body[txt_len] = '\0';
    c->json_len   = txt_len;
    c->fetched_ts = now;
    strncpy(c->repo, repo, sizeof(c->repo) - 1);
    c->repo[sizeof(c->repo) - 1] = '\0';
  }
  persist_cache_locked(repo, txt, txt_len);
  pthread_mutex_unlock(&g_lock);

  size_t wrapped_len = 0;
  char *wrapped = wrap_response(txt, txt_len, 0, now, NULL, &wrapped_len);
  free(txt);
  if(!wrapped) return serve_error(conn, 500, "alloc");
  return serve_buffer(conn, MHD_HTTP_OK, "application/json",
                      wrapped, wrapped_len, 1);
}


enum MHD_Result
releases_request(struct MHD_Connection *conn, const char *url) {
  if(!strcmp(url, "/api/releases")) return list_request(conn);
  return serve_error(conn, MHD_HTTP_NOT_FOUND, "no such endpoint");
}


static int
fetch_and_cache(const char *repo) {
  char url[256];
  snprintf(url, sizeof(url),
           "https://api.github.com/repos/%s/releases?per_page=100", repo);
  size_t raw_len = 0;
  uint8_t *raw = http_get(url, &raw_len);
  cJSON *slim = NULL;
  if(raw && raw_len) slim = slim_releases((const char*)raw, raw_len);
  free(raw);
  if(!slim) return -1;
  char *txt = cJSON_PrintUnformatted(slim);
  cJSON_Delete(slim);
  if(!txt) return -1;
  size_t txt_len = strlen(txt);
  pthread_mutex_lock(&g_lock);
  cache_t *c = cache_slot(repo);
  free(c->json_body);
  c->json_body = malloc(txt_len + 1);
  if(c->json_body) {
    memcpy(c->json_body, txt, txt_len);
    c->json_body[txt_len] = '\0';
    c->json_len   = txt_len;
    c->fetched_ts = time(NULL);
    strncpy(c->repo, repo, sizeof(c->repo) - 1);
    c->repo[sizeof(c->repo) - 1] = '\0';
  }
  persist_cache_locked(repo, txt, txt_len);
  pthread_mutex_unlock(&g_lock);
  free(txt);
  return 0;
}


static void*
releases_prefetch_thread(void *arg) {
  (void)arg;
  /* Backoff schedule. After the table runs out, stick at the last
     value forever (or until success). */
  static const int DELAYS[] = { 5, 10, 30, 60, 120 };

  /* Give the network stack a beat to settle before we try the first
     fetch — otherwise sceHttp on cold-boot sometimes times out. */
  sleep(8);

  for(int i = 0; ALLOWED_REPOS[i]; i++) {
    const char *repo = ALLOWED_REPOS[i];
    /* Skip if we already have a cache file on disk — auto-prefetch is
       only for first boot, not for refreshing existing data. */
    char path[256];
    cache_path_for(repo, path, sizeof(path));
    struct stat st;
    if(stat(path, &st) == 0 && st.st_size > 0) continue;

    int attempt = 0;
    for(;;) {
      if(fetch_and_cache(repo) == 0) break;
      int idx = attempt < (int)(sizeof(DELAYS)/sizeof(DELAYS[0]))
                  ? attempt : (int)(sizeof(DELAYS)/sizeof(DELAYS[0])) - 1;
      int wait = DELAYS[idx];
      attempt++;
      /* Sleep in 1-second chunks so a payload restart is responsive. */
      for(int s = 0; s < wait; s++) sleep(1);
    }
  }
  return NULL;
}


void
releases_init(void) {
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  pthread_create(&t, &a, releases_prefetch_thread, NULL);
  pthread_attr_destroy(&a);
  printf("releases: prefetch thread started\n");
}
