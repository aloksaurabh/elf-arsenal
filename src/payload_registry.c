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
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <microhttpd.h>

#include "payload_registry.h"
#include "websrv.h"
#include "ps5/http.h"
#include "third_party/cJSON.h"


#define REGISTRY_DIR     "/data/elf-arsenal"
#define REGISTRY_PATH    REGISTRY_DIR "/managed-payloads.json"
#define LATEST_CACHE_TTL (60 * 60)   /* 1 h cache for GitHub latest-release */
#define MAX_HITS         16          /* scanned install paths per entry */

#define SPLASH_AUTOLOADER \
  "/download0/cache/splash_screen/aHR0cHM6Ly93d3cueW91dHViZS5jb20vdHY=/ps5_autoloader"

#define DEFAULT_REGISTRY_JSON                                                \
  "{\n"                                                                      \
  "  \"payloads\": [\n"                                                      \
  "    {\n"                                                                  \
  "      \"name\": \"ps5-payload-manager\",\n"                               \
  "      \"description\": \"itsPLK's web-based PS5 payload dashboard\",\n"   \
  "      \"repo\": \"itsPLK/ps5-payload-manager\",\n"                        \
  "      \"asset_glob\": \"pldmgr-*.elf\",\n"                                \
  "      \"install_filename\": \"pldmgr.elf\",\n"                            \
  "      \"auto_update_on_boot\": true\n"                                    \
  "    }\n"                                                                  \
  "  ]\n"                                                                    \
  "}\n"


/* ---- in-memory cache of last GitHub-latest lookups ------------------ */
typedef struct {
  char   repo[96];
  char   tag[64];
  char   asset_name[160];
  char   asset_url[256];
  time_t fetched_ts;
} latest_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static latest_t        g_latest[16];



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

static enum MHD_Result
serve_cjson(struct MHD_Connection *conn, unsigned int status,
            cJSON *obj) {
  char *txt = cJSON_PrintUnformatted(obj);
  if(!txt) return serve_error(conn, 500, "alloc");
  return serve_buf(conn, status, "application/json",
                   txt, strlen(txt), 1);
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
read_file(const char *path, size_t *out_len) {
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
  if(out_len) *out_len = (size_t)st.st_size;
  return buf;
}

static int
file_size(const char *p) {
  struct stat st;
  if(stat(p, &st) != 0) return -1;
  return (int)st.st_size;
}


/* ----- glob matcher (just `*` + literal chars, case-sensitive) ------ */
static int
glob_match(const char *pat, const char *s) {
  while(*pat) {
    if(*pat == '*') {
      while(*pat == '*') pat++;
      if(!*pat) return 1;
      while(*s) {
        if(glob_match(pat, s)) return 1;
        s++;
      }
      return 0;
    }
    if(!*s || *s != *pat) return 0;
    s++; pat++;
  }
  return !*s;
}



static cJSON *
load_registry_or_default(void) {
  size_t n = 0;
  char *txt = read_file(REGISTRY_PATH, &n);
  if(!txt) {
    /* Bootstrap. We don't fail if /data isn't writable yet — return
       the in-memory default so /api/payloads/list still works. */
    write_file_atomic(REGISTRY_PATH,
                      DEFAULT_REGISTRY_JSON,
                      strlen(DEFAULT_REGISTRY_JSON));
    return cJSON_Parse(DEFAULT_REGISTRY_JSON);
  }
  cJSON *root = cJSON_ParseWithLength(txt, n);
  free(txt);
  if(!root) return cJSON_Parse(DEFAULT_REGISTRY_JSON);
  return root;
}

static int
save_registry(cJSON *root) {
  char *txt = cJSON_Print(root);
  if(!txt) return -1;
  int rc = write_file_atomic(REGISTRY_PATH, txt, strlen(txt));
  free(txt);
  return rc;
}



/* Append `path` to `out` if it's not already there and we have room. */
static void
maybe_add(const char *path, char hits[][384], int *n) {
  if(*n >= MAX_HITS) return;
  for(int i = 0; i < *n; i++)
    if(strcmp(hits[i], path) == 0) return;
  snprintf(hits[*n], 384, "%s", path);
  (*n)++;
}

static int
ends_with_iexact(const char *s, const char *suf) {
  size_t a = strlen(s), b = strlen(suf);
  if(b > a) return 0;
  for(size_t i = 0; i < b; i++) {
    char x = s[a - b + i], y = suf[i];
    if(tolower((unsigned char)x) != tolower((unsigned char)y)) return 0;
  }
  return 1;
}

static void
scan_autoloader_dir(const char *dir,
                    const char *install_filename,
                    const char *asset_glob,
                    char hits[][384], int *n) {
  if(!dir || !*dir) return;
  struct stat st;
  if(stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return;

  /* Direct hit on install_filename. */
  char primary[384];
  snprintf(primary, sizeof(primary), "%s/%s", dir, install_filename);
  if(file_size(primary) > 0) maybe_add(primary, hits, n);

  /* Permissive scan: anything matching asset_glob (e.g. pldmgr-*.elf). */
  DIR *d = opendir(dir);
  if(!d) return;
  struct dirent *e;
  while((e = readdir(d)) && *n < MAX_HITS) {
    if(e->d_name[0] == '.') continue;
    if(!glob_match(asset_glob, e->d_name)) continue;
    if(!ends_with_iexact(e->d_name, ".elf")) continue;
    char p[384];
    snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
    if(file_size(p) <= 0) continue;
    maybe_add(p, hits, n);
  }
  closedir(d);
}

static void
scan_all_locations(const char *install_filename, const char *asset_glob,
                   char hits[][384], int *n) {
  /* Internal autoloader dir (generic + per-title variants). */
  scan_autoloader_dir("/data/ps5_autoloader",
                      install_filename, asset_glob, hits, n);
  DIR *d = opendir("/data");
  if(d) {
    struct dirent *e;
    while((e = readdir(d)) && *n < MAX_HITS) {
      if(strncmp(e->d_name, "ps5_autoloader_", 15) != 0) continue;
      char p[384];
      snprintf(p, sizeof(p), "/data/%s", e->d_name);
      scan_autoloader_dir(p, install_filename, asset_glob, hits, n);
    }
    closedir(d);
  }

  /* YouTube-TV splash slot. */
  scan_autoloader_dir(SPLASH_AUTOLOADER,
                      install_filename, asset_glob, hits, n);

  /* USB roots — /mnt/usb0, /mnt/usb1, … plus /mnt/usbN/ps5_autoloader. */
  DIR *m = opendir("/mnt");
  if(m) {
    struct dirent *e;
    while((e = readdir(m)) && *n < MAX_HITS) {
      if(strncmp(e->d_name, "usb", 3) != 0) continue;
      char p[384];
      snprintf(p, sizeof(p), "/mnt/%s/ps5_autoloader", e->d_name);
      scan_autoloader_dir(p, install_filename, asset_glob, hits, n);
    }
    closedir(m);
  }

  scan_autoloader_dir("/data/pldmgr/payloads",
                      install_filename, asset_glob, hits, n);
}



/* "pldmgr-v0.1.1.elf" → "v0.1.1". Falls back to "" if no v[N.N.N]. */
static void
guess_version_from_filename(const char *path, char *out, size_t out_sz) {
  out[0] = 0;
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  const char *v = strstr(base, "-v");
  if(!v) v = strstr(base, "_v");
  if(!v) return;
  v += 1;  /* point at the 'v' */
  size_t w = 0;
  while(*v && w + 1 < out_sz) {
    if(*v == '.' || *v == '-' || *v == '_' || *v == '+' ||
       isalnum((unsigned char)*v)) {
      out[w++] = *v++;
    } else break;
  }
  /* Strip trailing ".elf" if it crept in. */
  while(w >= 4 && strcasecmp(out + w - 4, ".elf") == 0) w -= 4;
  out[w] = 0;
}


/* ----- GitHub latest-release lookup --------------------------------- */

static latest_t *
latest_slot(const char *repo) {
  for(int i = 0; i < (int)(sizeof(g_latest)/sizeof(g_latest[0])); i++) {
    if(strcmp(g_latest[i].repo, repo) == 0) return &g_latest[i];
  }
  /* Evict the oldest. */
  latest_t *victim = &g_latest[0];
  for(int i = 1; i < (int)(sizeof(g_latest)/sizeof(g_latest[0])); i++) {
    if(g_latest[i].fetched_ts < victim->fetched_ts) victim = &g_latest[i];
  }
  memset(victim, 0, sizeof(*victim));
  snprintf(victim->repo, sizeof(victim->repo), "%s", repo);
  return victim;
}

/* Fetch the newest release matching `asset_glob` for `repo`. Caller
   holds g_lock. Returns 0 on success and fills `out`. */
static int
github_latest_locked(const char *repo, const char *asset_glob,
                     latest_t *out, int force) {
  latest_t *slot = latest_slot(repo);
  time_t now = time(NULL);
  if(!force && slot->fetched_ts &&
     (now - slot->fetched_ts < LATEST_CACHE_TTL) &&
     slot->asset_url[0]) {
    *out = *slot;
    return 0;
  }

  char url[200];
  snprintf(url, sizeof(url),
           "https://api.github.com/repos/%s/releases?per_page=20", repo);

  size_t raw_len = 0;
  uint8_t *raw = NULL;
  /* Drop the lock while we do network I/O. */
  pthread_mutex_unlock(&g_lock);
  raw = http_get(url, &raw_len);
  pthread_mutex_lock(&g_lock);

  if(!raw || !raw_len) { free(raw); return -1; }

  cJSON *root = cJSON_ParseWithLength((const char *)raw, raw_len);
  free(raw);
  if(!root || !cJSON_IsArray(root)) { cJSON_Delete(root); return -1; }

  int found = 0;
  cJSON *rel;
  cJSON_ArrayForEach(rel, root) {
    cJSON *draft  = cJSON_GetObjectItem(rel, "draft");
    cJSON *prerel = cJSON_GetObjectItem(rel, "prerelease");
    if(cJSON_IsTrue(draft) || cJSON_IsTrue(prerel)) continue;
    cJSON *assets = cJSON_GetObjectItem(rel, "assets");
    cJSON *tag    = cJSON_GetObjectItem(rel, "tag_name");
    if(!cJSON_IsArray(assets) || !cJSON_IsString(tag)) continue;

    cJSON *a;
    cJSON_ArrayForEach(a, assets) {
      cJSON *name = cJSON_GetObjectItem(a, "name");
      cJSON *durl = cJSON_GetObjectItem(a, "browser_download_url");
      if(!cJSON_IsString(name) || !cJSON_IsString(durl)) continue;
      if(!glob_match(asset_glob, name->valuestring)) continue;

      slot->fetched_ts = now;
      snprintf(slot->tag,        sizeof(slot->tag),        "%s", tag->valuestring);
      snprintf(slot->asset_name, sizeof(slot->asset_name), "%s", name->valuestring);
      snprintf(slot->asset_url,  sizeof(slot->asset_url),  "%s", durl->valuestring);
      *out = *slot;
      found = 1;
      goto done;
    }
  }

done:
  cJSON_Delete(root);
  return found ? 0 : -1;
}



/* Download `url` and write its bytes to every path in `targets`. */
static int
install_to(const char *url, char targets[][384], int n_targets) {
  size_t blob_len = 0;
  uint8_t *blob = http_get(url, &blob_len);
  if(!blob || !blob_len) { free(blob); return -1; }

  int wrote = 0;
  for(int i = 0; i < n_targets; i++) {
    if(write_file_atomic(targets[i], blob, blob_len) == 0) wrote++;
  }
  free(blob);
  return wrote;
}


/* ----- per-entry state for /list ------------------------------------ */

static cJSON *
entry_state(cJSON *entry) {
  cJSON *name_j      = cJSON_GetObjectItem(entry, "name");
  cJSON *repo_j      = cJSON_GetObjectItem(entry, "repo");
  cJSON *glob_j      = cJSON_GetObjectItem(entry, "asset_glob");
  cJSON *install_j   = cJSON_GetObjectItem(entry, "install_filename");
  cJSON *desc_j      = cJSON_GetObjectItem(entry, "description");
  cJSON *auto_j      = cJSON_GetObjectItem(entry, "auto_update_on_boot");

  const char *name    = (cJSON_IsString(name_j))    ? name_j->valuestring    : "";
  const char *repo    = (cJSON_IsString(repo_j))    ? repo_j->valuestring    : "";
  const char *glob    = (cJSON_IsString(glob_j))    ? glob_j->valuestring    : "*.elf";
  const char *install = (cJSON_IsString(install_j)) ? install_j->valuestring : "";
  const char *desc    = (cJSON_IsString(desc_j))    ? desc_j->valuestring    : "";

  char hits[MAX_HITS][384];
  int  n = 0;
  scan_all_locations(install, glob, hits, &n);

  cJSON *paths_arr = cJSON_CreateArray();
  char installed_ver[64] = "";
  for(int i = 0; i < n; i++) {
    cJSON_AddItemToArray(paths_arr, cJSON_CreateString(hits[i]));
    if(!installed_ver[0])
      guess_version_from_filename(hits[i], installed_ver, sizeof(installed_ver));
  }

  cJSON *out = cJSON_CreateObject();
  cJSON_AddStringToObject(out, "name", name);
  cJSON_AddStringToObject(out, "description", desc);
  cJSON_AddStringToObject(out, "repo", repo);
  cJSON_AddStringToObject(out, "assetGlob", glob);
  cJSON_AddStringToObject(out, "installFilename", install);
  cJSON_AddBoolToObject(out, "autoUpdateOnBoot",
                        auto_j ? cJSON_IsTrue(auto_j) : 0);
  cJSON_AddItemToObject(out, "installedPaths", paths_arr);
  cJSON_AddStringToObject(out, "installedVersion", installed_ver);
  cJSON_AddBoolToObject(out, "installed", n > 0);

  /* Try a (cached) latest-release peek. Don't fail if it errors —
     just leave latestTag/latestAsset empty. */
  latest_t latest = {0};
  pthread_mutex_lock(&g_lock);
  int got = github_latest_locked(repo, glob, &latest, 0);
  pthread_mutex_unlock(&g_lock);
  cJSON_AddStringToObject(out, "latestTag",   got == 0 ? latest.tag        : "");
  cJSON_AddStringToObject(out, "latestAsset", got == 0 ? latest.asset_name : "");

  return out;
}



static enum MHD_Result
list_request(struct MHD_Connection *conn) {
  cJSON *reg = load_registry_or_default();
  if(!reg) return serve_error(conn, 500, "registry parse failed");

  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "ok", 1);
  cJSON *items = cJSON_AddArrayToObject(out, "items");

  cJSON *list = cJSON_GetObjectItem(reg, "payloads");
  if(cJSON_IsArray(list)) {
    cJSON *e;
    cJSON_ArrayForEach(e, list) {
      cJSON_AddItemToArray(items, entry_state(e));
    }
  }
  cJSON_Delete(reg);
  enum MHD_Result rc = serve_cjson(conn, MHD_HTTP_OK, out);
  cJSON_Delete(out);
  return rc;
}

static cJSON *
find_entry(cJSON *reg, const char *name) {
  cJSON *list = cJSON_GetObjectItem(reg, "payloads");
  if(!cJSON_IsArray(list)) return NULL;
  cJSON *e;
  cJSON_ArrayForEach(e, list) {
    cJSON *n = cJSON_GetObjectItem(e, "name");
    if(cJSON_IsString(n) && strcmp(n->valuestring, name) == 0) return e;
  }
  return NULL;
}

static enum MHD_Result
update_request(struct MHD_Connection *conn) {
  const char *name = MHD_lookup_connection_value(conn,
                       MHD_GET_ARGUMENT_KIND, "name");
  if(!name || !*name)
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "missing 'name'");

  cJSON *reg = load_registry_or_default();
  if(!reg) return serve_error(conn, 500, "registry parse failed");
  cJSON *e = find_entry(reg, name);
  if(!e) { cJSON_Delete(reg); return serve_error(conn, 404, "no such payload"); }

  cJSON *repo_j    = cJSON_GetObjectItem(e, "repo");
  cJSON *glob_j    = cJSON_GetObjectItem(e, "asset_glob");
  cJSON *install_j = cJSON_GetObjectItem(e, "install_filename");
  if(!cJSON_IsString(repo_j) || !cJSON_IsString(glob_j) ||
     !cJSON_IsString(install_j)) {
    cJSON_Delete(reg);
    return serve_error(conn, 500, "registry entry missing repo/asset_glob/install_filename");
  }

  latest_t latest = {0};
  pthread_mutex_lock(&g_lock);
  int got = github_latest_locked(repo_j->valuestring, glob_j->valuestring,
                                 &latest, 1);
  pthread_mutex_unlock(&g_lock);
  if(got != 0) {
    cJSON_Delete(reg);
    return serve_error(conn, MHD_HTTP_BAD_GATEWAY,
                       "GitHub latest-release lookup failed");
  }

  char hits[MAX_HITS][384];
  int n = 0;
  scan_all_locations(install_j->valuestring, glob_j->valuestring, hits, &n);
  if(n == 0) {
    char p[384];
    snprintf(p, sizeof(p), "/data/ps5_autoloader/%s", install_j->valuestring);
    snprintf(hits[0], sizeof(hits[0]), "%s", p);
    n = 1;
    struct stat st_pld;
    if(stat("/data/pldmgr/payloads", &st_pld) == 0 &&
       S_ISDIR(st_pld.st_mode)) {
      char q[384];
      snprintf(q, sizeof(q), "/data/pldmgr/payloads/%s",
               install_j->valuestring);
      snprintf(hits[1], sizeof(hits[1]), "%s", q);
      n = 2;
    }
  }

  int wrote = install_to(latest.asset_url, hits, n);

  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "ok", wrote > 0);
  cJSON_AddStringToObject(out, "name", name);
  cJSON_AddStringToObject(out, "tag", latest.tag);
  cJSON_AddStringToObject(out, "asset", latest.asset_name);
  cJSON_AddNumberToObject(out, "wrote", wrote);
  cJSON_AddNumberToObject(out, "attempted", n);
  cJSON *paths = cJSON_AddArrayToObject(out, "paths");
  for(int i = 0; i < n; i++)
    cJSON_AddItemToArray(paths, cJSON_CreateString(hits[i]));

  cJSON_Delete(reg);
  enum MHD_Result rc = serve_cjson(conn,
      wrote > 0 ? MHD_HTTP_OK : MHD_HTTP_INTERNAL_SERVER_ERROR, out);
  cJSON_Delete(out);
  return rc;
}

static enum MHD_Result
refresh_latest_request(struct MHD_Connection *conn) {
  cJSON *reg = load_registry_or_default();
  if(!reg) return serve_error(conn, 500, "registry parse failed");
  cJSON *list = cJSON_GetObjectItem(reg, "payloads");
  if(cJSON_IsArray(list)) {
    cJSON *e;
    cJSON_ArrayForEach(e, list) {
      cJSON *repo_j = cJSON_GetObjectItem(e, "repo");
      cJSON *glob_j = cJSON_GetObjectItem(e, "asset_glob");
      if(!cJSON_IsString(repo_j) || !cJSON_IsString(glob_j)) continue;
      latest_t tmp = {0};
      pthread_mutex_lock(&g_lock);
      github_latest_locked(repo_j->valuestring, glob_j->valuestring,
                           &tmp, 1);
      pthread_mutex_unlock(&g_lock);
    }
  }
  cJSON_Delete(reg);
  return list_request(conn);
}

static enum MHD_Result
auto_toggle_request(struct MHD_Connection *conn) {
  const char *name = MHD_lookup_connection_value(conn,
                       MHD_GET_ARGUMENT_KIND, "name");
  const char *on   = MHD_lookup_connection_value(conn,
                       MHD_GET_ARGUMENT_KIND, "on");
  if(!name || !*name)
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "missing 'name'");
  cJSON *reg = load_registry_or_default();
  if(!reg) return serve_error(conn, 500, "registry parse failed");
  cJSON *e = find_entry(reg, name);
  if(!e) { cJSON_Delete(reg); return serve_error(conn, 404, "no such payload"); }
  int want = (on && strcmp(on, "0") != 0);

  cJSON *cur = cJSON_GetObjectItem(e, "auto_update_on_boot");
  if(cur) cJSON_DeleteItemFromObject(e, "auto_update_on_boot");
  cJSON_AddBoolToObject(e, "auto_update_on_boot", want);

  save_registry(reg);
  cJSON_Delete(reg);
  return list_request(conn);
}


static int
sanitise_field(const char *s, size_t max_len, int allow_slash,
               int allow_dot, int allow_glob, int allow_dash_under) {
  if(!s || !*s) return -1;
  size_t n = strlen(s);
  if(n > max_len) return -1;
  for(size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if(isalnum(c)) continue;
    if(allow_slash && c == '/')     continue;
    if(allow_dot   && c == '.')     continue;
    if(allow_glob  && (c == '*' || c == '?')) continue;
    if(allow_dash_under && (c == '-' || c == '_')) continue;
    return -1;
  }
  /* Reject anything that looks like path traversal. */
  if(strstr(s, "..")) return -1;
  return 0;
}

/* Normalise a "github.com/owner/repo" or full URL into "owner/repo".
   Writes into `out`. Returns 0 on success. */
static int
normalise_repo(const char *in, char *out, size_t out_sz) {
  if(!in) return -1;
  const char *p = in;
  /* Strip protocol + host if user pasted a full URL. */
  const char *needle = "github.com/";
  const char *hit = strstr(p, needle);
  if(hit) p = hit + strlen(needle);
  /* Trim trailing slashes and ".git". */
  size_t n = strlen(p);
  while(n > 0 && (p[n-1] == '/' || p[n-1] == '\n' || p[n-1] == '\r')) n--;
  if(n >= 4 && memcmp(p + n - 4, ".git", 4) == 0) n -= 4;
  if(n == 0 || n >= out_sz) return -1;
  memcpy(out, p, n);
  out[n] = 0;
  if(sanitise_field(out, out_sz - 1, 1, 1, 0, 1) != 0) return -1;
  if(!strchr(out, '/')) return -1;
  return 0;
}

static enum MHD_Result
add_request(struct MHD_Connection *conn) {
  const char *name_q     = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "name");
  const char *repo_q     = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "repo");
  const char *glob_q     = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "asset_glob");
  const char *install_q  = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "install_filename");
  const char *desc_q     = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "description");
  const char *auto_q     = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "auto_update_on_boot");

  if(sanitise_field(name_q,    64, 0, 1, 0, 1) != 0)
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "invalid name");
  if(sanitise_field(glob_q,    96, 0, 1, 1, 1) != 0)
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "invalid asset_glob");
  if(sanitise_field(install_q, 96, 0, 1, 0, 1) != 0)
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "invalid install_filename");
  char repo[96];
  if(normalise_repo(repo_q, repo, sizeof(repo)) != 0)
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "invalid repo");
  int want_auto = (auto_q && strcmp(auto_q, "0") != 0);

  cJSON *reg = load_registry_or_default();
  if(!reg) return serve_error(conn, 500, "registry parse failed");
  if(find_entry(reg, name_q)) {
    cJSON_Delete(reg);
    return serve_error(conn, MHD_HTTP_CONFLICT, "name already in registry");
  }
  cJSON *list = cJSON_GetObjectItem(reg, "payloads");
  if(!cJSON_IsArray(list)) {
    list = cJSON_CreateArray();
    cJSON_AddItemToObject(reg, "payloads", list);
  }
  cJSON *entry = cJSON_CreateObject();
  cJSON_AddStringToObject(entry, "name",                name_q);
  cJSON_AddStringToObject(entry, "description",         desc_q && *desc_q ? desc_q : "");
  cJSON_AddStringToObject(entry, "repo",                repo);
  cJSON_AddStringToObject(entry, "asset_glob",          glob_q);
  cJSON_AddStringToObject(entry, "install_filename",    install_q);
  cJSON_AddBoolToObject(entry,  "auto_update_on_boot",  want_auto);
  cJSON_AddItemToArray(list, entry);

  if(save_registry(reg) != 0) {
    cJSON_Delete(reg);
    return serve_error(conn, 500, "failed to persist registry");
  }
  cJSON_Delete(reg);
  return list_request(conn);
}

static enum MHD_Result
remove_request(struct MHD_Connection *conn) {
  const char *name = MHD_lookup_connection_value(conn,
                       MHD_GET_ARGUMENT_KIND, "name");
  if(!name || !*name)
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "missing 'name'");

  cJSON *reg = load_registry_or_default();
  if(!reg) return serve_error(conn, 500, "registry parse failed");
  cJSON *list = cJSON_GetObjectItem(reg, "payloads");
  if(!cJSON_IsArray(list)) {
    cJSON_Delete(reg);
    return serve_error(conn, 404, "registry has no entries");
  }
  int idx = -1, i = 0;
  cJSON *e;
  cJSON_ArrayForEach(e, list) {
    cJSON *n = cJSON_GetObjectItem(e, "name");
    if(cJSON_IsString(n) && strcmp(n->valuestring, name) == 0) {
      idx = i;
      break;
    }
    i++;
  }
  if(idx < 0) {
    cJSON_Delete(reg);
    return serve_error(conn, 404, "no such payload");
  }
  cJSON_DeleteItemFromArray(list, idx);
  if(save_registry(reg) != 0) {
    cJSON_Delete(reg);
    return serve_error(conn, 500, "failed to persist registry");
  }
  cJSON_Delete(reg);
  return list_request(conn);
}

static enum MHD_Result
lookup_request(struct MHD_Connection *conn) {
  const char *repo_q = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "repo");
  const char *glob_q = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "asset_glob");
  char repo[96];
  if(normalise_repo(repo_q, repo, sizeof(repo)) != 0)
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "invalid repo");
  if(sanitise_field(glob_q, 96, 0, 1, 1, 1) != 0)
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "invalid asset_glob");

  latest_t latest = {0};
  pthread_mutex_lock(&g_lock);
  int got = github_latest_locked(repo, glob_q, &latest, 1);
  pthread_mutex_unlock(&g_lock);

  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "ok", got == 0);
  cJSON_AddStringToObject(out, "repo",      repo);
  cJSON_AddStringToObject(out, "assetGlob", glob_q);
  if(got == 0) {
    cJSON_AddStringToObject(out, "tag",        latest.tag);
    cJSON_AddStringToObject(out, "assetName",  latest.asset_name);
    cJSON_AddStringToObject(out, "assetUrl",   latest.asset_url);
  } else {
    cJSON_AddStringToObject(out, "error",
      "GitHub lookup failed — wrong repo, no matching asset, or network down");
  }
  enum MHD_Result rc = serve_cjson(conn,
      got == 0 ? MHD_HTTP_OK : MHD_HTTP_BAD_GATEWAY, out);
  cJSON_Delete(out);
  return rc;
}


enum MHD_Result
payload_registry_request(struct MHD_Connection *conn, const char *url) {
  if(!strcmp(url, "/api/payloads/list"))           return list_request(conn);
  if(!strcmp(url, "/api/payloads/update"))         return update_request(conn);
  if(!strcmp(url, "/api/payloads/refresh-latest")) return refresh_latest_request(conn);
  if(!strcmp(url, "/api/payloads/auto-toggle"))    return auto_toggle_request(conn);
  if(!strcmp(url, "/api/payloads/add"))            return add_request(conn);
  if(!strcmp(url, "/api/payloads/remove"))         return remove_request(conn);
  if(!strcmp(url, "/api/payloads/lookup"))         return lookup_request(conn);
  return serve_error(conn, MHD_HTTP_NOT_FOUND, "no such endpoint");
}


/* ----- on-boot scan + update ---------------------------------------- */

#define PAYLOAD_BOOT_DELAY_SECONDS  45

static void
do_boot_sweep(void) {
  cJSON *reg = load_registry_or_default();
  if(!reg) return;
  cJSON *list = cJSON_GetObjectItem(reg, "payloads");
  if(!cJSON_IsArray(list)) { cJSON_Delete(reg); return; }

  cJSON *e;
  cJSON_ArrayForEach(e, list) {
    cJSON *auto_j    = cJSON_GetObjectItem(e, "auto_update_on_boot");
    cJSON *repo_j    = cJSON_GetObjectItem(e, "repo");
    cJSON *glob_j    = cJSON_GetObjectItem(e, "asset_glob");
    cJSON *install_j = cJSON_GetObjectItem(e, "install_filename");
    if(!auto_j || !cJSON_IsTrue(auto_j))            continue;
    if(!cJSON_IsString(repo_j) || !cJSON_IsString(glob_j) ||
       !cJSON_IsString(install_j))                  continue;

    char hits[MAX_HITS][384];
    int  n = 0;
    scan_all_locations(install_j->valuestring, glob_j->valuestring,
                       hits, &n);
    if(n == 0) continue;

    latest_t latest = {0};
    pthread_mutex_lock(&g_lock);
    int got = github_latest_locked(repo_j->valuestring,
                                   glob_j->valuestring,
                                   &latest, 1);
    pthread_mutex_unlock(&g_lock);
    if(got != 0) continue;

    /* If the installed version already matches the latest tag, skip
       the download. */
    char have[64] = "";
    guess_version_from_filename(hits[0], have, sizeof(have));
    if(have[0] && strcmp(have, latest.tag) == 0) continue;

    install_to(latest.asset_url, hits, n);
  }
  cJSON_Delete(reg);
}

#include <sys/syscall.h>

static void *
boot_update_thread(void *arg) {
  (void)arg;
  /* Best-effort thread name for klog grepping. SYS_thr_set_name is
     the PS5 / FreeBSD-ish syscall — fine if it errors. */
  syscall(SYS_thr_set_name, -1, "pldmgr-boot");
  sleep(PAYLOAD_BOOT_DELAY_SECONDS);
  fprintf(stderr,
          "payload_registry: boot sweep starting after %ds delay\n",
          PAYLOAD_BOOT_DELAY_SECONDS);
  do_boot_sweep();
  fprintf(stderr, "payload_registry: boot sweep finished\n");
  return NULL;
}

/* -------- /api/payloads/upload — local-file (PC browser) install --- */

typedef struct {
  int    fd;
  char   stage_path[384];   /* /data/elf-arsenal/pkgs/payload-uploads/<name> */
  char   install_filename[96];
  char   display_name[96];
  size_t bytes;
  int    init_failed;
  int    auto_update;       /* mirror of ?auto=1 (kept for completeness) */
  char   error[160];
} payload_upload_t;

void
payload_upload_free(void *state) {
  payload_upload_t *u = state;
  if(!u) return;
  if(u->fd >= 0) { close(u->fd); u->fd = -1; }
  free(u);
}


static enum MHD_Result
respond_upload_err(struct MHD_Connection *conn,
                   payload_upload_t *u, void **state,
                   unsigned int status, const char *msg) {
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", 0);
  cJSON_AddStringToObject(r, "error", msg ? msg : u->error);
  enum MHD_Result ret = serve_cjson(conn, status, r);
  cJSON_Delete(r);
  if(u->stage_path[0]) unlink(u->stage_path);
  payload_upload_free(u);
  *state = NULL;
  return ret;
}

enum MHD_Result
payload_upload_request(struct MHD_Connection *conn,
                       const char *upload_data,
                       size_t *upload_data_size,
                       void **state) {
  payload_upload_t *u = *state;

  /* Phase 1: first call — set up. */
  if(!u) {
    u = calloc(1, sizeof(*u));
    if(!u) return MHD_NO;
    u->fd = -1;
    *state = u;

    const char *install = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "install_filename");
    const char *name    = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "name");
    const char *autoq   = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "auto_update_on_boot");
    u->auto_update = (autoq && strcmp(autoq, "0") != 0);

    if(sanitise_field(install, 95, 0, 1, 0, 1) != 0) {
      strncpy(u->error,
              "missing or invalid install_filename (alnum + . _ - only)",
              sizeof(u->error) - 1);
      u->init_failed = 1;
      return MHD_YES;
    }
    /* Require .elf suffix so we never drop a .pkg into the autoloader
       dir by mistake. */
    size_t il = strlen(install);
    if(il < 4 || strcasecmp(install + il - 4, ".elf") != 0) {
      strncpy(u->error, "install_filename must end in .elf",
              sizeof(u->error) - 1);
      u->init_failed = 1;
      return MHD_YES;
    }
    snprintf(u->install_filename, sizeof(u->install_filename), "%s", install);

    if(name && *name && sanitise_field(name, 63, 0, 1, 0, 1) == 0) {
      snprintf(u->display_name, sizeof(u->display_name), "%s", name);
    } else {
      /* Default the registry name to the filename minus .elf. */
      snprintf(u->display_name, sizeof(u->display_name), "%s", install);
      size_t dl = strlen(u->display_name);
      if(dl >= 4) u->display_name[dl - 4] = 0;
    }

    snprintf(u->stage_path, sizeof(u->stage_path),
             "/data/elf-arsenal/pkgs/payload-uploads/%s",
             u->install_filename);
    mkdirs_for(u->stage_path);

    u->fd = open(u->stage_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(u->fd < 0) {
      snprintf(u->error, sizeof(u->error), "open %s: %s",
               u->stage_path, strerror(errno));
      u->init_failed = 1;
      u->stage_path[0] = 0;
    }
    return MHD_YES;
  }

  /* Phase 2: streamed body chunks. */
  if(*upload_data_size > 0) {
    if(!u->init_failed && u->fd >= 0) {
      size_t want = *upload_data_size, off = 0;
      while(off < want) {
        ssize_t w = write(u->fd, upload_data + off, want - off);
        if(w <= 0) {
          snprintf(u->error, sizeof(u->error), "write: %s", strerror(errno));
          u->init_failed = 1;
          break;
        }
        off += (size_t)w;
      }
      u->bytes += off;
    }
    *upload_data_size = 0;
    return MHD_YES;
  }

  /* Phase 3: end-of-body — finalize. */
  if(u->fd >= 0) { close(u->fd); u->fd = -1; }

  if(u->init_failed) {
    return respond_upload_err(conn, u, state, MHD_HTTP_BAD_REQUEST,
                              u->error[0] ? u->error : "upload init failed");
  }
  if(u->bytes == 0) {
    return respond_upload_err(conn, u, state, MHD_HTTP_BAD_REQUEST,
                              "empty upload");
  }

  char targets[MAX_HITS][384];
  int n = 0;
  snprintf(targets[n++], sizeof(targets[0]),
           "/data/ps5_autoloader/%s", u->install_filename);
  struct stat st_pld;
  if(stat("/data/pldmgr/payloads", &st_pld) == 0 &&
     S_ISDIR(st_pld.st_mode) && n < MAX_HITS) {
    snprintf(targets[n++], sizeof(targets[0]),
             "/data/pldmgr/payloads/%s", u->install_filename);
  }

  size_t blob_len = 0;
  char *blob = read_file(u->stage_path, &blob_len);
  int wrote = 0;
  if(blob) {
    for(int i = 0; i < n; i++) {
      if(write_file_atomic(targets[i], blob, blob_len) == 0) wrote++;
    }
    free(blob);
  }

  cJSON *reg = load_registry_or_default();
  int registry_ok = 0;
  if(reg) {
    if(!find_entry(reg, u->display_name)) {
      cJSON *list = cJSON_GetObjectItem(reg, "payloads");
      if(!cJSON_IsArray(list)) {
        list = cJSON_CreateArray();
        cJSON_AddItemToObject(reg, "payloads", list);
      }
      cJSON *entry = cJSON_CreateObject();
      cJSON_AddStringToObject(entry, "name",                u->display_name);
      cJSON_AddStringToObject(entry, "description",         "Local upload (no GitHub source)");
      cJSON_AddStringToObject(entry, "repo",                "");
      cJSON_AddStringToObject(entry, "asset_glob",          u->install_filename);
      cJSON_AddStringToObject(entry, "install_filename",    u->install_filename);
      cJSON_AddBoolToObject(entry,   "auto_update_on_boot", 0);
      cJSON_AddItemToArray(list, entry);
      registry_ok = (save_registry(reg) == 0);
    } else {
      /* Existing entry — leave registry alone, the on-disk file just
         got refreshed. Treat that as success for the response. */
      registry_ok = 1;
    }
    cJSON_Delete(reg);
  }

  /* Wipe the staging copy — it's been propagated to the real
     install paths. */
  unlink(u->stage_path);

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", wrote > 0 && registry_ok);
  cJSON_AddStringToObject(r, "name",            u->display_name);
  cJSON_AddStringToObject(r, "installFilename", u->install_filename);
  cJSON_AddNumberToObject(r, "size",            (double)u->bytes);
  cJSON_AddNumberToObject(r, "wrote",           wrote);
  cJSON_AddNumberToObject(r, "attempted",       n);
  cJSON *paths = cJSON_AddArrayToObject(r, "paths");
  for(int i = 0; i < n; i++) cJSON_AddItemToArray(paths, cJSON_CreateString(targets[i]));
  if(wrote == 0) {
    cJSON_AddStringToObject(r, "error",
        "Upload landed but every install target rejected the write. "
        "Check that /data/ps5_autoloader exists and is writable.");
  }
  enum MHD_Result ret = serve_cjson(conn,
      (wrote > 0 && registry_ok) ? MHD_HTTP_OK
                                 : MHD_HTTP_INTERNAL_SERVER_ERROR, r);
  cJSON_Delete(r);
  payload_upload_free(u);
  *state = NULL;
  return ret;
}


void
payload_registry_boot_update(void) {
  pthread_t tid;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if(pthread_create(&tid, &attr, boot_update_thread, NULL) != 0) {
    perror("payload_registry_boot_update: pthread_create");
    /* Fall back to inline if we couldn't fork — better than nothing. */
    sleep(PAYLOAD_BOOT_DELAY_SECONDS);
    do_boot_sweep();
  }
  pthread_attr_destroy(&attr);
}
