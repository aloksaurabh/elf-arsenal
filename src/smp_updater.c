
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <microhttpd.h>

#include "zipread.h"

#include "offline_pack.h"
#include "smp_updater.h"
#include "smp_meta.h"
#include "fpkg_db.h"
#include "sys.h"
#include "ps5/http.h"
#include "third_party/cJSON.h"
#include "websrv.h"


#define SMP_INSTALL_DIR  "/data/shadowmount"
#define SMP_INSTALL_PATH "/data/shadowmount/shadowmountplus.elf"

#define SMP_LATEST_TAG    "1.6beta9"
#define SMP_LATEST_ASSET  "shadowmountplus.elf"
#define SMP_LATEST_DIRECT \
  "https://github.com/drakmor/shadowMountPlus/releases/download/" \
  SMP_LATEST_TAG "/" SMP_LATEST_ASSET

/* Legacy aliases — both point at the latest release. */
#define SMP_103_TAG     SMP_LATEST_TAG
#define SMP_103_DIRECT  SMP_LATEST_DIRECT
#define SMP_104_TAG     SMP_LATEST_TAG
#define SMP_104_DIRECT  SMP_LATEST_DIRECT


static enum MHD_Result
serve_buffer(struct MHD_Connection *conn, unsigned int status,
             const char *mime, void *data, size_t size, int free_after) {
  enum MHD_Result ret = MHD_NO;
  struct MHD_Response *resp;
  enum MHD_ResponseMemoryMode mode = free_after ? MHD_RESPMEM_MUST_FREE
                                                : MHD_RESPMEM_PERSISTENT;
  if((resp=MHD_create_response_from_buffer(size, data, mode))) {
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
serve_json(struct MHD_Connection *conn, unsigned int status, cJSON *obj) {
  char *txt = cJSON_PrintUnformatted(obj);
  if(!txt) return serve_buffer(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                               "application/json",
                               "{\"error\":\"alloc\"}", 17, 0);
  return serve_buffer(conn, status, "application/json", txt, strlen(txt), 1);
}


static enum MHD_Result
serve_error(struct MHD_Connection *conn, unsigned int status, const char *msg) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "ok", 0);
  cJSON_AddStringToObject(o, "error", msg);
  enum MHD_Result ret = serve_json(conn, status, o);
  cJSON_Delete(o);
  return ret;
}


static int
atomic_write(const char *path, const uint8_t *buf, size_t len) {
  char tmp[256];
  if(snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;

  int fd = open(tmp, O_WRONLY|O_CREAT|O_TRUNC, 0755);
  if(fd < 0) return -1;
  size_t off = 0;
  while(off < len) {
    ssize_t n = write(fd, buf + off, len - off);
    if(n <= 0) { close(fd); unlink(tmp); return -1; }
    off += (size_t)n;
  }
  close(fd);

  if(rename(tmp, path) != 0) { unlink(tmp); return -1; }
  chmod(path, 0755);
  return 0;
}


static const char*
version_to_url(const char *v, const char **tag_out, const char **label_out) {
  if(!v) return NULL;
  if(!strcmp(v, "103") || !strcmp(v, "104") ||
     !strcmp(v, "latest") || !strcmp(v, SMP_LATEST_TAG)) {
    if(tag_out)   *tag_out   = SMP_LATEST_TAG;
    if(label_out) *label_out = "drakmor SMP " SMP_LATEST_TAG " (latest)";
    return SMP_LATEST_DIRECT;
  }
  return NULL;
}


int
smp_install_direct(const char *version) {
  const char *url = version_to_url(version, NULL, NULL);
  if(!url) return -1;

  size_t elf_len = 0;
  uint8_t *elf = http_get(url, &elf_len);
  if(!elf || elf_len < 64 ||
     elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F') {
    free(elf);
    return -1;
  }

  mkdir(SMP_INSTALL_DIR, 0755);
  int rc = atomic_write(SMP_INSTALL_PATH, elf, elf_len);
  free(elf);
  return rc;
}


/* Pull the asset_url for the FIRST asset whose name ends in `ext`
   (lowercase, including the leading dot). Caller frees. */
static char*
find_release_asset_url(const char *json_text, size_t json_len,
                       const char *ext) {
  cJSON *root = cJSON_ParseWithLength(json_text, json_len);
  if(!root) return NULL;
  char *out = NULL;
  size_t el = strlen(ext);
  cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
  if(cJSON_IsArray(assets)) {
    cJSON *e;
    cJSON_ArrayForEach(e, assets) {
      cJSON *n = cJSON_GetObjectItemCaseSensitive(e, "name");
      cJSON *u = cJSON_GetObjectItemCaseSensitive(e, "browser_download_url");
      if(!cJSON_IsString(n) || !cJSON_IsString(u)) continue;
      const char *name = n->valuestring;
      size_t nl = strlen(name);
      if(nl >= el && !strcasecmp(name + nl - el, ext)) {
        out = strdup(u->valuestring);
        break;
      }
    }
  }
  cJSON_Delete(root);
  return out;
}


static int
extract_named_from_zip(const uint8_t *zip_data, size_t zip_len,
                       const char *target_basename,
                       uint8_t **out_buf, size_t *out_len,
                       char *err, size_t err_size) {
  *out_buf = NULL;
  *out_len = 0;

  ezip *a = ezip_open_mem(zip_data, zip_len);
  if(ezip_failed(a)) {
    snprintf(err, err_size, "zip open: %s", ezip_error(a));
    ezip_close(a);
    return -1;
  }

  ezip_entry entry;
  while(ezip_next(a, &entry) == 1) {
    const char *name = entry.path;
    if(!name || !*name || entry.is_dir) continue;
    const char *bn = strrchr(name, '/');
    bn = bn ? bn + 1 : name;
    if(strcasecmp(bn, target_basename) != 0) continue;

    if(entry.size <= 0 || entry.size > 64 * 1024 * 1024) {
      snprintf(err, err_size, "zip entry %s size implausible: %lld",
               target_basename, (long long)entry.size);
      ezip_close(a);
      return -1;
    }
    size_t got = 0;
    *out_buf = ezip_extract_heap(a, &got);
    if(!*out_buf) {
      snprintf(err, err_size, "zip read: %s", ezip_error(a));
      ezip_close(a);
      return -1;
    }
    *out_len = got;
    ezip_close(a);
    return 0;
  }

  snprintf(err, err_size, "zip does not contain '%s'", target_basename);
  ezip_close(a);
  return -1;
}




static enum MHD_Result
install_request(struct MHD_Connection *conn) {
  const char *want_tag = MHD_lookup_connection_value(conn,
                                MHD_GET_ARGUMENT_KIND, "tag");
  const char *legacy_v = MHD_lookup_connection_value(conn,
                                MHD_GET_ARGUMENT_KIND, "version");
  const char *autorestart = MHD_lookup_connection_value(conn,
                                MHD_GET_ARGUMENT_KIND, "autorestart");

  const char *tag = NULL;
  if(want_tag && *want_tag) {
    tag = want_tag;
  } else if(legacy_v && (!strcmp(legacy_v, "103") ||
                         !strcmp(legacy_v, "104") ||
                         !strcasecmp(legacy_v, "latest"))) {
    tag = SMP_LATEST_TAG;
  } else if(legacy_v && *legacy_v) {
    tag = legacy_v;
  } else {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
        "missing ?tag=<release-tag> (e.g. 1.6beta3, 1.5beta6-fix1)");
  }

  if(offline_pack_get_enabled() && offline_pack_is_installed()) {
    uint8_t *eb = NULL; size_t el = 0;
    if(offline_pack_get_asset("smp", tag, "shadowmountplus.elf", &eb, &el) == 0) {
      mkdir(SMP_INSTALL_DIR, 0755);
      int wr = atomic_write(SMP_INSTALL_PATH, eb, el);
      free(eb);
      if(wr == 0) {
        if(autorestart && (*autorestart == '1' || *autorestart == 't')) {
          sys_smp_restart();
        }
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", 1);
        cJSON_AddStringToObject(r, "source", "offline-pack");
        cJSON_AddStringToObject(r, "tag", tag);
        cJSON_AddStringToObject(r, "path", SMP_INSTALL_PATH);
        cJSON_AddNumberToObject(r, "size", (double)el);
        return serve_json(conn, MHD_HTTP_OK, r);
      }
    }
  }

  /* Hit GitHub for the release's JSON, pick the first .elf asset. */
  char releases_url[256];
  snprintf(releases_url, sizeof(releases_url),
           "https://api.github.com/repos/drakmor/ShadowMountPlus/"
           "releases/tags/%s", tag);

  size_t json_len = 0;
  uint8_t *json   = http_get(releases_url, &json_len);
  char *asset_url = NULL;
  int   from_zip  = 0;
  if(json && json_len) {
    asset_url = find_release_asset_url((const char*)json, json_len, ".elf");
    if(!asset_url) {
      asset_url = find_release_asset_url((const char*)json, json_len, ".zip");
      from_zip  = (asset_url != NULL);
    }
  }
  free(json);

  if(!asset_url) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r,   "ok", 0);
    cJSON_AddStringToObject(r, "tagRequested", tag);
    cJSON_AddStringToObject(r, "error",
        "release has no .elf or .zip asset on GitHub");
    enum MHD_Result rc = serve_json(conn, MHD_HTTP_NOT_FOUND, r);
    cJSON_Delete(r);
    return rc;
  }

  /* Pull the asset bytes — same path for .elf and .zip; we sort
     out which one we got below. */
  size_t blob_len = 0;
  uint8_t *blob = http_get(asset_url, &blob_len);
  if(!blob || blob_len < 64) {
    free(blob);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r,   "ok", 0);
    cJSON_AddStringToObject(r, "error",
        "download failed — GitHub didn't return a valid SMP asset");
    cJSON_AddStringToObject(r, "url", asset_url);
    free(asset_url);
    enum MHD_Result rc = serve_json(conn, MHD_HTTP_BAD_GATEWAY, r);
    cJSON_Delete(r);
    return rc;
  }

  uint8_t *elf     = blob;
  size_t   elf_len = blob_len;
  if(from_zip) {
    char zerr[256] = "";
    uint8_t *extracted = NULL;
    size_t   extracted_len = 0;
    if(extract_named_from_zip(blob, blob_len, "shadowmountplus.elf",
                              &extracted, &extracted_len,
                              zerr, sizeof(zerr)) != 0) {
      /* legacy filename fallback */
      if(extract_named_from_zip(blob, blob_len, "shadowmount.elf",
                                &extracted, &extracted_len,
                                zerr, sizeof(zerr)) != 0) {
        free(blob);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r,   "ok", 0);
        cJSON_AddStringToObject(r, "error", zerr);
        cJSON_AddStringToObject(r, "url", asset_url);
        free(asset_url);
        enum MHD_Result rc = serve_json(conn, MHD_HTTP_BAD_GATEWAY, r);
        cJSON_Delete(r);
        return rc;
      }
    }
    free(blob);
    elf     = extracted;
    elf_len = extracted_len;
  }

  if(elf_len < 64 || elf[0] != 0x7f ||
     elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F') {
    free(elf);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r,   "ok", 0);
    cJSON_AddStringToObject(r, "error",
                            "extracted payload is not a valid ELF (bad magic)");
    cJSON_AddStringToObject(r, "url", asset_url);
    cJSON_AddNumberToObject(r, "size", (double)elf_len);
    cJSON_AddBoolToObject(r,   "fromZip", from_zip);
    free(asset_url);
    enum MHD_Result rc = serve_json(conn, MHD_HTTP_BAD_GATEWAY, r);
    cJSON_Delete(r);
    return rc;
  }

  /* Touch only the .elf — config.ini, daemon.lock, smp_icon.png stay. */
  mkdir(SMP_INSTALL_DIR, 0755);
  int wr = atomic_write(SMP_INSTALL_PATH, elf, elf_len);
  free(elf);

  cJSON *r = cJSON_CreateObject();
  if(wr != 0) {
    cJSON_AddBoolToObject(r,   "ok", 0);
    cJSON_AddStringToObject(r, "error", strerror(errno));
    cJSON_AddStringToObject(r, "path", SMP_INSTALL_PATH);
    cJSON_AddStringToObject(r, "url", asset_url);
    free(asset_url);
    enum MHD_Result rc = serve_json(conn,
                                    MHD_HTTP_INTERNAL_SERVER_ERROR, r);
    cJSON_Delete(r);
    return rc;
  }

  int restart = 1;
  if(autorestart && (!strcmp(autorestart, "0") ||
                     !strcasecmp(autorestart, "false"))) {
    restart = 0;
  }
  int restarted = restart ? (sys_smp_restart() == 0) : 0;

  cJSON_AddBoolToObject(r,   "ok", 1);
  cJSON_AddStringToObject(r, "tag", tag);
  cJSON_AddStringToObject(r, "path", SMP_INSTALL_PATH);
  cJSON_AddNumberToObject(r, "size", (double)elf_len);
  cJSON_AddStringToObject(r, "url", asset_url);
  cJSON_AddBoolToObject  (r, "restarted", restarted);
  cJSON_AddStringToObject(r, "note",
      restarted
        ? "ShadowMountPlus installed and daemon restarted with the new build."
        : "ShadowMountPlus installed. Click 'Restart SMP' or re-send "
          "the Elf Arsenal payload to apply.");
  free(asset_url);
  enum MHD_Result rc = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return rc;
}


static enum MHD_Result
info_request(struct MHD_Connection *conn) {
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "path", SMP_INSTALL_PATH);
  cJSON_AddStringToObject(r, "latestTag", SMP_LATEST_TAG);
  cJSON_AddStringToObject(r, "latestUrl", SMP_LATEST_DIRECT);
  cJSON_AddStringToObject(r, "version103Tag", SMP_LATEST_TAG);
  cJSON_AddStringToObject(r, "version103Url", SMP_LATEST_DIRECT);
  cJSON_AddStringToObject(r, "version104Tag", SMP_LATEST_TAG);
  cJSON_AddStringToObject(r, "version104Url", SMP_LATEST_DIRECT);
  struct stat st;
  if(stat(SMP_INSTALL_PATH, &st) == 0) {
    cJSON_AddBoolToObject(r,   "exists", 1);
    cJSON_AddNumberToObject(r, "size",   (double)st.st_size);
    cJSON_AddNumberToObject(r, "mtime",  (double)st.st_mtime);
  } else {
    cJSON_AddBoolToObject(r, "exists", 0);
  }
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


static enum MHD_Result
reset_request(struct MHD_Connection *conn) {
  cJSON *r = cJSON_CreateObject();
  if(unlink(SMP_INSTALL_PATH) == 0) {
    cJSON_AddBoolToObject(r,   "ok", 1);
    cJSON_AddStringToObject(r, "note",
        "Deleted /data/shadowmount/shadowmountplus.elf only. "
        "config.ini, daemon.lock and smp_icon.png are preserved. "
        "Reinstall via Settings -> Install SMP picker before the next "
        "Elf Arsenal boot — SMP is no longer bundled.");
  } else if(errno == ENOENT) {
    cJSON_AddBoolToObject(r,   "ok", 1);
    cJSON_AddStringToObject(r, "note",
        "shadowmountplus.elf already absent. SMP isn't bundled in "
        "this Elf Arsenal build — install via Settings -> Install "
        "SMP picker before the next boot.");
  } else {
    cJSON_AddBoolToObject(r,   "ok", 0);
    cJSON_AddStringToObject(r, "error", strerror(errno));
  }
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}



#define SMP_CONFIG_PATH "/data/shadowmount/config.ini"
#define SMP_SENTINEL_DIR "/data/shadowmount/empty"


static const char * const SMP_DEFAULT_SCAN_PATHS[] = {
  /* Internal */
  "/data/homebrew", "/data/etaHEN/games",
  /* Extended Storage / M.2 */
  "/mnt/ext0/homebrew",  "/mnt/ext0/etaHEN/games",
  "/mnt/ext1/homebrew",  "/mnt/ext1/etaHEN/games",
  /* USB subfolders */
  "/mnt/usb0/homebrew",  "/mnt/usb1/homebrew",
  "/mnt/usb2/homebrew",  "/mnt/usb3/homebrew",
  "/mnt/usb4/homebrew",  "/mnt/usb5/homebrew",
  "/mnt/usb6/homebrew",  "/mnt/usb7/homebrew",
  "/mnt/usb0/etaHEN/games", "/mnt/usb1/etaHEN/games",
  "/mnt/usb2/etaHEN/games", "/mnt/usb3/etaHEN/games",
  "/mnt/usb4/etaHEN/games", "/mnt/usb5/etaHEN/games",
  "/mnt/usb6/etaHEN/games", "/mnt/usb7/etaHEN/games",
  /* Root mounts */
  "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
  "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7",
  "/mnt/ext0", "/mnt/ext1",
  NULL,
};



#include <stdatomic.h>

static atomic_int g_smp_defaults_on = 1;     /* default ON */

/* SMP config.ini tunables — written by rewrite_scanpath_block(). */
static atomic_int g_smp_debug                    = 1;
static atomic_int g_smp_quiet_mode               = 0;
static atomic_int g_smp_kstuff_auto_toggle       = 1;
static atomic_int g_smp_kstuff_crash_detection   = 1;
static atomic_int g_smp_kstuff_pause_delay_image  = 25;
static atomic_int g_smp_kstuff_pause_delay_direct = 15;

int  smp_cfg_get_debug(void)                  { return atomic_load(&g_smp_debug); }
void smp_cfg_set_debug(int v)                 { atomic_store(&g_smp_debug, v ? 1 : 0); }
int  smp_cfg_get_quiet_mode(void)             { return atomic_load(&g_smp_quiet_mode); }
void smp_cfg_set_quiet_mode(int v)            { atomic_store(&g_smp_quiet_mode, v ? 1 : 0); }
int  smp_cfg_get_kstuff_auto_toggle(void)     { return atomic_load(&g_smp_kstuff_auto_toggle); }
void smp_cfg_set_kstuff_auto_toggle(int v)    { atomic_store(&g_smp_kstuff_auto_toggle, v ? 1 : 0); }
int  smp_cfg_get_kstuff_crash_detection(void) { return atomic_load(&g_smp_kstuff_crash_detection); }
void smp_cfg_set_kstuff_crash_detection(int v){ atomic_store(&g_smp_kstuff_crash_detection, v ? 1 : 0); }
int  smp_cfg_get_pause_delay_image(void)      { return atomic_load(&g_smp_kstuff_pause_delay_image); }
void smp_cfg_set_pause_delay_image(int v)     { atomic_store(&g_smp_kstuff_pause_delay_image, v < 0 ? 0 : v > 3600 ? 3600 : v); }
int  smp_cfg_get_pause_delay_direct(void)     { return atomic_load(&g_smp_kstuff_pause_delay_direct); }
void smp_cfg_set_pause_delay_direct(int v)    { atomic_store(&g_smp_kstuff_pause_delay_direct, v < 0 ? 0 : v > 3600 ? 3600 : v); }

#define MAX_MANUAL_PATHS    64
#define MAX_PATH_LEN        256
static char g_manual_paths[MAX_MANUAL_PATHS][MAX_PATH_LEN];
static int  g_manual_count = 0;

int
sys_smp_defaults_get(void) {
  return atomic_load(&g_smp_defaults_on);
}

void
sys_smp_defaults_set(int on) {
  atomic_store(&g_smp_defaults_on, on ? 1 : 0);
}

/* Serialise the manual paths to a single comma-separated string for
   config.ini. Caller-owned buffer. */
void
sys_smp_manual_paths_serialize(char *out, size_t out_size) {
  out[0] = '\0';
  size_t off = 0;
  for(int i = 0; i < g_manual_count; i++) {
    size_t need = strlen(g_manual_paths[i]) + (i ? 1 : 0);
    if(off + need + 1 >= out_size) break;
    if(i) out[off++] = ',';
    memcpy(out + off, g_manual_paths[i], strlen(g_manual_paths[i]));
    off += strlen(g_manual_paths[i]);
    out[off] = '\0';
  }
}

/* Replace the manual list from a comma-separated string. */
void
sys_smp_manual_paths_load(const char *csv) {
  g_manual_count = 0;
  if(!csv || !*csv) return;
  const char *p = csv;
  while(*p && g_manual_count < MAX_MANUAL_PATHS) {
    const char *comma = strchr(p, ',');
    size_t len = comma ? (size_t)(comma - p) : strlen(p);
    if(len > 0 && len < MAX_PATH_LEN) {
      memcpy(g_manual_paths[g_manual_count], p, len);
      g_manual_paths[g_manual_count][len] = '\0';
      g_manual_count++;
    }
    if(!comma) break;
    p = comma + 1;
  }
}

/* Call cb(path, arg) for every active SMP scan path — default paths
   (when defaults are on) followed by any manual paths. */
void
smp_foreach_scan_path(void (*cb)(const char *, void *), void *arg) {
  if(atomic_load(&g_smp_defaults_on)) {
    for(int i = 0; SMP_DEFAULT_SCAN_PATHS[i]; i++)
      cb(SMP_DEFAULT_SCAN_PATHS[i], arg);
  }
  for(int i = 0; i < g_manual_count; i++)
    cb(g_manual_paths[i], arg);
}


/* Slurp config.ini into a heap buffer (caller frees). Returns NULL on
   I/O failure or if the file is huge (>64 KB). */
static char*
read_config(size_t *out_len) {
  FILE *f = fopen(SMP_CONFIG_PATH, "r");
  if(!f) return NULL;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if(sz < 0 || sz > 64 * 1024) { fclose(f); return NULL; }
  char *buf = malloc((size_t)sz + 1);
  if(!buf) { fclose(f); return NULL; }
  size_t n = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[n] = '\0';
  if(out_len) *out_len = n;
  return buf;
}


static int
write_config_atomic(const char *body) {
  mkdir("/data/shadowmount", 0755);
  const char *tmp = SMP_CONFIG_PATH ".tmp";
  FILE *o = fopen(tmp, "w");
  if(!o) return -1;
  size_t n = strlen(body);
  if(fwrite(body, 1, n, o) != n) { fclose(o); unlink(tmp); return -1; }
  fclose(o);
  return rename(tmp, SMP_CONFIG_PATH);
}


/* Strip leading + trailing whitespace in place. */
static char*
trim(char *s) {
  while(*s == ' ' || *s == '\t') s++;
  char *e = s + strlen(s);
  while(e > s && (e[-1] == ' ' || e[-1] == '\t' ||
                  e[-1] == '\r' || e[-1] == '\n')) e--;
  *e = '\0';
  return s;
}


static int
rewrite_scanpath_block(void) {
  size_t len = 0;
  char *raw = read_config(&len);

  /* Estimate buffer: existing config + 32 KB headroom for default
     paths block. */
  size_t cap = (raw ? len : 0) + 32 * 1024;
  char *out = calloc(1, cap);
  if(!out) { free(raw); return -1; }

  /* Copy every non-scanpath line verbatim. */
  if(raw) {
    char *line = raw;
    while(line && *line) {
      char *eol = strpbrk(line, "\r\n");
      size_t L = eol ? (size_t)(eol - line) : strlen(line);
      char tmp[1024];
      int drop = 0;
      if(L < sizeof(tmp)) {
        memcpy(tmp, line, L); tmp[L] = '\0';
        char *t = trim(tmp);
        if(strncmp(t, "scanpath=", 9) == 0) drop = 1;
        if(strncmp(t, "debug=", 6) == 0) drop = 1;
        if(strncmp(t, "quiet_mode=", 11) == 0) drop = 1;
        if(strncmp(t, "kstuff_game_auto_toggle=", 24) == 0) drop = 1;
        if(strncmp(t, "kstuff_crash_detection=", 23) == 0) drop = 1;
        if(strncmp(t, "kstuff_pause_delay_image_seconds=", 33) == 0) drop = 1;
        if(strncmp(t, "kstuff_pause_delay_direct_seconds=", 34) == 0) drop = 1;
      }
      if(!drop) {
        size_t off = strlen(out);
        if(off + L + 2 < cap) {
          memcpy(out + off, line, L);
          out[off + L] = '\n';
          out[off + L + 1] = '\0';
        }
      }
      if(!eol) break;
      line = eol + 1;
      while(*line == '\r' || *line == '\n') line++;
    }
    free(raw);
  }

  int defaults_on = sys_smp_defaults_get();

  /* Build the new scanpath= block. */
  if(defaults_on && g_manual_count == 0) {
    /* No-op. SMP's compile-time defaults take over. */
  } else if(defaults_on && g_manual_count > 0) {
    for(int i = 0; SMP_DEFAULT_SCAN_PATHS[i]; i++) {
      size_t off = strlen(out);
      int n = snprintf(out + off, cap - off, "scanpath=%s\n",
                       SMP_DEFAULT_SCAN_PATHS[i]);
      if(n < 0 || (size_t)n >= cap - off) break;
    }
    for(int i = 0; i < g_manual_count; i++) {
      size_t off = strlen(out);
      int n = snprintf(out + off, cap - off, "scanpath=%s\n",
                       g_manual_paths[i]);
      if(n < 0 || (size_t)n >= cap - off) break;
    }
  } else if(!defaults_on && g_manual_count > 0) {
    for(int i = 0; i < g_manual_count; i++) {
      size_t off = strlen(out);
      int n = snprintf(out + off, cap - off, "scanpath=%s\n",
                       g_manual_paths[i]);
      if(n < 0 || (size_t)n >= cap - off) break;
    }
  } else {
    mkdir(SMP_SENTINEL_DIR, 0755);
    size_t off = strlen(out);
    snprintf(out + off, cap - off, "scanpath=%s\n", SMP_SENTINEL_DIR);
  }

  /* Write SMP config.ini tunables from Elf Arsenal settings. */
  {
    size_t off = strlen(out);
    snprintf(out + off, cap - off,
             "debug=%d\n"
             "quiet_mode=%d\n"
             "kstuff_game_auto_toggle=%d\n"
             "kstuff_crash_detection=%d\n"
             "kstuff_pause_delay_image_seconds=%d\n"
             "kstuff_pause_delay_direct_seconds=%d\n",
             smp_cfg_get_debug(),
             smp_cfg_get_quiet_mode(),
             smp_cfg_get_kstuff_auto_toggle(),
             smp_cfg_get_kstuff_crash_detection(),
             smp_cfg_get_pause_delay_image(),
             smp_cfg_get_pause_delay_direct());
  }

  int wrc = write_config_atomic(out);
  free(out);
  return wrc;
}


/* GET /api/smp/scanpath/list — full state for the Settings UI. */
static enum MHD_Result
scanpath_list_request(struct MHD_Connection *conn) {
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "defaultsOn", sys_smp_defaults_get());

  cJSON *defs = cJSON_AddArrayToObject(r, "defaults");
  for(int i = 0; SMP_DEFAULT_SCAN_PATHS[i]; i++)
    cJSON_AddItemToArray(defs, cJSON_CreateString(SMP_DEFAULT_SCAN_PATHS[i]));

  cJSON *man = cJSON_AddArrayToObject(r, "manual");
  for(int i = 0; i < g_manual_count; i++)
    cJSON_AddItemToArray(man, cJSON_CreateString(g_manual_paths[i]));

  cJSON_AddStringToObject(r, "configPath",  SMP_CONFIG_PATH);
  cJSON_AddStringToObject(r, "sentinelDir", SMP_SENTINEL_DIR);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


/* GET /api/smp/scanpath/defaults?on=0|1 — toggle defaults. */
static enum MHD_Result
scanpath_defaults_request(struct MHD_Connection *conn) {
  const char *on = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                               "on");
  const char *ar = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                               "autorestart");
  if(!on)
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "missing ?on=0 or ?on=1");
  int want = (!strcmp(on, "1") || !strcasecmp(on, "true")) ? 1 : 0;
  sys_smp_defaults_set(want);
  /* Persist to /data/elf-arsenal/config.ini. */
  extern void config_save(void);
  config_save();
  /* Rewrite SMP config.ini to reflect the new state. */
  if(rewrite_scanpath_block() != 0)
    return serve_error(conn, 500, "config write failed");

  int restarted = 0;
  if(ar && (!strcmp(ar, "1") || !strcasecmp(ar, "true"))) {
    restarted = (sys_smp_restart() == 0);
  }
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", 1);
  cJSON_AddBoolToObject(r, "defaultsOn", want);
  cJSON_AddBoolToObject(r, "restarted",  restarted);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


static enum MHD_Result
scanpath_add_request(struct MHD_Connection *conn) {
  const char *path = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                                 "path");
  const char *ar   = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                                 "autorestart");
  if(!path || !path[0])
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                       "missing ?path=/some/dir");
  if(path[0] != '/')
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                       "path must be absolute (start with /)");
  struct stat st;
  if(stat(path, &st) != 0)
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                       "path does not exist on this console");
  if(!S_ISDIR(st.st_mode))
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                       "path is not a directory");

  /* Dedupe — either against existing manual or against the defaults
     when defaults are on. */
  int already = 0;
  for(int i = 0; i < g_manual_count; i++) {
    if(!strcmp(g_manual_paths[i], path)) { already = 1; break; }
  }
  if(!already) {
    if(g_manual_count >= MAX_MANUAL_PATHS)
      return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                         "manual scan-path list is full");
    if(strlen(path) >= MAX_PATH_LEN)
      return serve_error(conn, MHD_HTTP_BAD_REQUEST, "path too long");
    strncpy(g_manual_paths[g_manual_count], path, MAX_PATH_LEN - 1);
    g_manual_paths[g_manual_count][MAX_PATH_LEN - 1] = '\0';
    g_manual_count++;

    extern void config_save(void);
    config_save();
  }
  if(rewrite_scanpath_block() != 0)
    return serve_error(conn, 500, "config write failed");

  int restarted = 0;
  if(ar && (!strcmp(ar, "1") || !strcasecmp(ar, "true"))) {
    restarted = (sys_smp_restart() == 0);
  }

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r,   "ok", 1);
  cJSON_AddStringToObject(r, "path", path);
  cJSON_AddBoolToObject(r,   "alreadyPresent", already);
  cJSON_AddBoolToObject(r,   "restarted", restarted);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


/* GET /api/smp/scanpath/remove?path=/...&autorestart=0|1 */
static enum MHD_Result
scanpath_remove_request(struct MHD_Connection *conn) {
  const char *path = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                                 "path");
  const char *ar   = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                                 "autorestart");
  if(!path || !path[0])
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "missing ?path=…");

  int removed = 0;
  for(int i = 0; i < g_manual_count; i++) {
    if(!strcmp(g_manual_paths[i], path)) {
      for(int j = i; j < g_manual_count - 1; j++) {
        memcpy(g_manual_paths[j], g_manual_paths[j + 1], MAX_PATH_LEN);
      }
      g_manual_count--;
      removed = 1;
      break;
    }
  }
  if(removed) {
    extern void config_save(void);
    config_save();
  }
  if(rewrite_scanpath_block() != 0)
    return serve_error(conn, 500, "config write failed");

  int restarted = 0;
  if(ar && (!strcmp(ar, "1") || !strcasecmp(ar, "true"))) {
    restarted = (sys_smp_restart() == 0);
  }

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r,   "ok", 1);
  cJSON_AddNumberToObject(r, "removed", removed ? 1 : 0);
  cJSON_AddBoolToObject(r,   "restarted", restarted);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


/* GET /api/smp/scanpath/clear — drop every manual path. */
static enum MHD_Result
scanpath_clear_request(struct MHD_Connection *conn) {
  int removed = g_manual_count;
  g_manual_count = 0;
  extern void config_save(void);
  config_save();
  if(rewrite_scanpath_block() != 0)
    return serve_error(conn, 500, "config write failed");
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", 1);
  cJSON_AddNumberToObject(r, "removed", removed);
  cJSON_AddStringToObject(r, "note",
      "All manual paths cleared. Defaults toggle controls whether "
      "SMP's compile-time scan list still applies.");
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


/* GET /api/smp/restart — kill + respawn the daemon. */
static enum MHD_Result
restart_request(struct MHD_Connection *conn) {
  int rc = sys_smp_restart();
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", rc == 0);
  cJSON_AddBoolToObject(r, "running", sys_smp_is_running());
  if(rc != 0) cJSON_AddStringToObject(r, "error", "spawn failed");
  enum MHD_Result ret = serve_json(conn,
      rc == 0 ? MHD_HTTP_OK : MHD_HTTP_INTERNAL_SERVER_ERROR, r);
  cJSON_Delete(r);
  return ret;
}


/* GET /api/smp/toggle?on=0|1 */
static enum MHD_Result
toggle_request(struct MHD_Connection *conn) {
  const char *on = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                               "on");
  if(!on)
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "missing ?on=0 or ?on=1");
  int want = (!strcmp(on, "1") || !strcasecmp(on, "true")) ? 1 : 0;
  int rc = sys_smp_set_enabled(want);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", rc >= 0);
  cJSON_AddBoolToObject(r, "running", sys_smp_is_running());
  if(rc < 0) cJSON_AddStringToObject(r, "error",
      want ? "spawn failed" : "kill failed");
  enum MHD_Result ret = serve_json(conn,
      rc >= 0 ? MHD_HTTP_OK : MHD_HTTP_INTERNAL_SERVER_ERROR, r);
  cJSON_Delete(r);
  return ret;
}


/* GET /api/smp/state — info_request + running flag combined. */
static enum MHD_Result
state_request(struct MHD_Connection *conn) {
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "running",  sys_smp_is_running());
  cJSON_AddStringToObject(r, "configPath", SMP_CONFIG_PATH);
  cJSON_AddStringToObject(r, "installPath", SMP_INSTALL_PATH);
  struct stat st;
  cJSON_AddBoolToObject(r, "userInstalled",
      stat(SMP_INSTALL_PATH, &st) == 0 && st.st_size > 64);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


/* GET /api/smp/meta — current healer stats. */
static enum MHD_Result
meta_status_request(struct MHD_Connection *conn) {
  smp_meta_stats_t s;
  smp_meta_get_stats(&s);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject  (r, "running",       s.running);
  cJSON_AddNumberToObject(r, "pollSeconds",   s.poll_seconds);
  cJSON_AddNumberToObject(r, "lastRunUnix",   (double)s.last_run_unix);
  cJSON_AddNumberToObject(r, "gamesScanned",  s.games_scanned);
  cJSON_AddNumberToObject(r, "iconsHealed",   s.icons_healed);
  cJSON_AddNumberToObject(r, "picsHealed",    s.pics_healed);
  cJSON_AddNumberToObject(r, "jsonHealed",    s.json_healed);
  cJSON_AddNumberToObject(r, "stillMissing",  s.still_missing);
  cJSON_AddStringToObject(r, "lastMissing",   s.last_missing);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


/* GET /api/smp/meta/run — kick a sweep immediately and return new stats. */
static enum MHD_Result
meta_run_now_request(struct MHD_Connection *conn) {
  smp_meta_run_now();
  /* Don't block waiting for the sweep — the caller can poll
     /api/smp/meta a couple of seconds later for fresh numbers. */
  return meta_status_request(conn);
}


/* GET /api/smp/meta/backup — snapshot the fpkg registration now. */
static enum MHD_Result
meta_backup_now_request(struct MHD_Connection *conn) {
  fpkg_db_full_snapshot();
  return meta_status_request(conn);
}


/* GET /api/smp/fpkg/rebuild?src=current|prev — restore the PS4 fpkg listings
   from the chosen backup set, then refresh the home screen. */
static enum MHD_Result
fpkg_rebuild_request(struct MHD_Connection *conn) {
  const char *src = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "src");
  int use_prev = (src && !strcmp(src, "prev"));
  int n = 0;
  fpkg_protect_files();
  fpkg_db_repair_src(use_prev, &n);
  fpkg_sweep_orphans();
  sys_refresh_shellui();
  return meta_status_request(conn);
}


/* GET /api/smp/fpkg/unlock — release the protected CUSA folders back to 0777
   so games can be moved/deleted through the PS5 UI again. */
static enum MHD_Result
fpkg_unlock_request(struct MHD_Connection *conn) {
  fpkg_unprotect_files();
  /* Signal fpkg_guard daemon (separate process) to stop its 30s re-lock cycle. */
  int fd = open("/tmp/.fpkg_guard_unlock", O_CREAT|O_WRONLY|O_TRUNC, 0644);
  if(fd >= 0) close(fd);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", 1);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


/* GET /api/smp/meta/poll?seconds=N — clamp to [5, 600], persist via
   the runtime setter. */
static enum MHD_Result
meta_poll_request(struct MHD_Connection *conn) {
  const char *v = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                              "seconds");
  if(!v) return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                            "missing ?seconds=N (5..600)");
  int n = atoi(v);
  smp_meta_set_poll_seconds(n);
  return meta_status_request(conn);
}


static enum MHD_Result
smp_config_request(struct MHD_Connection *conn) {
  /* Apply any query params that were sent. */
  const char *p;
#define QBOOL(param, setter) \
  if((p = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, param))) \
    setter(strcmp(p,"0") && strcasecmp(p,"false") && strcasecmp(p,"off"));
#define QINT(param, setter) \
  if((p = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, param))) \
    setter((int)strtol(p, NULL, 10));

  QBOOL("debug",           smp_cfg_set_debug)
  QBOOL("quiet_mode",      smp_cfg_set_quiet_mode)
  QBOOL("kstuff_toggle",   smp_cfg_set_kstuff_auto_toggle)
  QBOOL("crash_detect",    smp_cfg_set_kstuff_crash_detection)
  QINT ("pause_image",     smp_cfg_set_pause_delay_image)
  QINT ("pause_direct",    smp_cfg_set_pause_delay_direct)
#undef QBOOL
#undef QINT

  /* Persist to Elf Arsenal config and rewrite SMP config.ini. */
  extern void config_save(void);
  config_save();
  rewrite_scanpath_block();

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "debug",                smp_cfg_get_debug());
  cJSON_AddBoolToObject(r, "quietMode",            smp_cfg_get_quiet_mode());
  cJSON_AddBoolToObject(r, "kstuffAutoToggle",     smp_cfg_get_kstuff_auto_toggle());
  cJSON_AddBoolToObject(r, "kstuffCrashDetection", smp_cfg_get_kstuff_crash_detection());
  cJSON_AddNumberToObject(r, "pauseDelayImage",    smp_cfg_get_pause_delay_image());
  cJSON_AddNumberToObject(r, "pauseDelayDirect",   smp_cfg_get_pause_delay_direct());
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


enum MHD_Result
smp_updater_request(struct MHD_Connection *conn, const char *url) {
  if(!strcmp(url, "/api/smp"))         return info_request(conn);
  if(!strcmp(url, "/api/smp/info"))    return info_request(conn);
  if(!strcmp(url, "/api/smp/state"))   return state_request(conn);
  if(!strcmp(url, "/api/smp/install")) return install_request(conn);
  if(!strcmp(url, "/api/smp/reset"))   return reset_request(conn);
  if(!strcmp(url, "/api/smp/restart")) return restart_request(conn);
  if(!strcmp(url, "/api/smp/toggle"))  return toggle_request(conn);
  if(!strcmp(url, "/api/smp/scanpath/list"))     return scanpath_list_request(conn);
  if(!strcmp(url, "/api/smp/scanpath/add"))      return scanpath_add_request(conn);
  if(!strcmp(url, "/api/smp/scanpath/remove"))   return scanpath_remove_request(conn);
  if(!strcmp(url, "/api/smp/scanpath/clear"))    return scanpath_clear_request(conn);
  if(!strcmp(url, "/api/smp/scanpath/defaults")) return scanpath_defaults_request(conn);
  if(!strcmp(url, "/api/smp/config"))    return smp_config_request(conn);
  if(!strcmp(url, "/api/smp/meta"))        return meta_status_request(conn);
  if(!strcmp(url, "/api/smp/meta/run"))    return meta_run_now_request(conn);
  if(!strcmp(url, "/api/smp/meta/backup")) return meta_backup_now_request(conn);
  if(!strcmp(url, "/api/smp/fpkg/rebuild")) return fpkg_rebuild_request(conn);
  if(!strcmp(url, "/api/smp/fpkg/unlock"))  return fpkg_unlock_request(conn);
  if(!strcmp(url, "/api/smp/meta/poll"))   return meta_poll_request(conn);
  return serve_error(conn, MHD_HTTP_NOT_FOUND, "no such endpoint");
}
