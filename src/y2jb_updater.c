
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "ps5/http.h"
#include "third_party/cJSON.h"
#include "websrv.h"
#include "y2jb_updater.h"


#define AUTOLOADER_NAME      "ps5_autoloader"
#define ELF_WITH_ETAHEN      "elf-arsenal.elf"
#define ELF_NO_ETAHEN        "sonic-loader-no-etahen.elf"
/* aHR0cHM6Ly93d3cueW91dHViZS5jb20vdHY= = base64("https://www.youtube.com/tv") */
#define YT_SPLASH_BASE64     "aHR0cHM6Ly93d3cueW91dHViZS5jb20vdHY="

#define GITEA_LATEST_URL \
  "https://git.etawen.dev/api/v1/repos/soniciso/elf-arsenal/releases/latest"

#define MAX_DIRS  64


typedef struct {
  char path[256];          /* full path of the ps5_autoloader dir */
  int  has_with_etahen;
  int  has_no_etahen;
  long size_with_etahen;
  long size_no_etahen;
} y2jb_dir_t;



static int
dir_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}


static long
file_size(const char *p) {
  struct stat st;
  if(stat(p, &st) != 0) return -1;
  return (long)st.st_size;
}


/* If `dir` contains either Elf Arsenal variant, fill in `out` and
   return 1. Otherwise return 0. */
static int
inspect_autoloader_dir(const char *dir, y2jb_dir_t *out) {
  char p1[384], p2[384];
  snprintf(p1, sizeof(p1), "%s/%s", dir, ELF_WITH_ETAHEN);
  snprintf(p2, sizeof(p2), "%s/%s", dir, ELF_NO_ETAHEN);

  long s1 = file_size(p1);
  long s2 = file_size(p2);
  if(s1 < 0 && s2 < 0) return 0;

  strncpy(out->path, dir, sizeof(out->path) - 1);
  out->path[sizeof(out->path) - 1] = 0;
  out->has_with_etahen   = (s1 > 0);
  out->has_no_etahen     = (s2 > 0);
  out->size_with_etahen  = (s1 > 0) ? s1 : 0;
  out->size_no_etahen    = (s2 > 0) ? s2 : 0;
  return 1;
}


static void
probe_base(const char *base, y2jb_dir_t *out, int *out_n) {
  if(!dir_exists(base)) return;

  /* Generic dir. */
  char generic[384];
  snprintf(generic, sizeof(generic), "%s/%s", base, AUTOLOADER_NAME);
  if(*out_n < MAX_DIRS) {
    y2jb_dir_t entry = {0};
    if(dir_exists(generic) && inspect_autoloader_dir(generic, &entry)) {
      out[(*out_n)++] = entry;
    }
  }

  /* Per-title variants — anything starting with "ps5_autoloader_". */
  DIR *d = opendir(base);
  if(!d) return;
  struct dirent *e;
  while((e = readdir(d)) && *out_n < MAX_DIRS) {
    if(e->d_name[0] == '.') continue;
    if(strncmp(e->d_name, AUTOLOADER_NAME "_",
               sizeof(AUTOLOADER_NAME))) continue;
    char path[384];
    snprintf(path, sizeof(path), "%s/%s", base, e->d_name);
    if(!dir_exists(path)) continue;
    y2jb_dir_t entry = {0};
    if(inspect_autoloader_dir(path, &entry)) {
      out[(*out_n)++] = entry;
    }
  }
  closedir(d);
}


static int
y2jb_scan(y2jb_dir_t *out) {
  int n = 0;

  /* USB drives — highest priority per autoload.js. */
  for(int i = 0; i <= 7 && n < MAX_DIRS; i++) {
    char base[64];
    snprintf(base, sizeof(base), "/mnt/usb%d", i);
    probe_base(base, out, &n);
  }

  /* Internal /data. */
  if(n < MAX_DIRS) probe_base("/data", out, &n);

  if(n < MAX_DIRS) {
    const char *pldmgr_dir = "/data/pldmgr/payloads/sonic-loader";
    y2jb_dir_t entry = {0};
    if(dir_exists(pldmgr_dir) && inspect_autoloader_dir(pldmgr_dir, &entry)) {
      out[n++] = entry;
    }
  }

  /* YT-sandbox paths intentionally excluded. */
  return n;
}



typedef struct {
  char *with_etahen_url;
  char *no_etahen_url;
  char *tag;
  long  size_with_etahen;   // -1 if unknown
  long  size_no_etahen;     // -1 if unknown
} latest_release_t;


static void
release_free(latest_release_t *r) {
  if(!r) return;
  free(r->with_etahen_url);
  free(r->no_etahen_url);
  free(r->tag);
  memset(r, 0, sizeof(*r));
}


/* Fetch the latest release JSON from Gitea, extract the two ELF
   asset URLs. Returns 0 on success. */
static int
fetch_latest_release(latest_release_t *out) {
  size_t len = 0;
  uint8_t *body = http_get(GITEA_LATEST_URL, &len);
  if(!body || len == 0) {
    free(body);
    return -1;
  }
  char *json = realloc(body, len + 1);
  if(!json) { free(body); return -1; }
  json[len] = 0;

  cJSON *root = cJSON_Parse(json);
  free(json);
  if(!root) return -1;

  memset(out, 0, sizeof(*out));
  out->size_with_etahen = -1;
  out->size_no_etahen   = -1;

  cJSON *tag = cJSON_GetObjectItemCaseSensitive(root, "tag_name");
  if(cJSON_IsString(tag) && tag->valuestring) {
    out->tag = strdup(tag->valuestring);
  }

  cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
  if(cJSON_IsArray(assets)) {
    cJSON *a;
    cJSON_ArrayForEach(a, assets) {
      cJSON *name = cJSON_GetObjectItemCaseSensitive(a, "name");
      cJSON *url  = cJSON_GetObjectItemCaseSensitive(a, "browser_download_url");
      cJSON *sz   = cJSON_GetObjectItemCaseSensitive(a, "size");
      if(!cJSON_IsString(name) || !cJSON_IsString(url)) continue;
      long size = cJSON_IsNumber(sz) ? (long)sz->valuedouble : -1;
      if(!strcmp(name->valuestring, ELF_NO_ETAHEN)) {
        free(out->no_etahen_url);
        out->no_etahen_url = strdup(url->valuestring);
        out->size_no_etahen = size;
      } else if(!strcmp(name->valuestring, ELF_WITH_ETAHEN)) {
        free(out->with_etahen_url);
        out->with_etahen_url = strdup(url->valuestring);
        out->size_with_etahen = size;
      }
    }
  }

  cJSON_Delete(root);
  if(!out->with_etahen_url && !out->no_etahen_url) {
    release_free(out);
    return -1;
  }
  return 0;
}


static int
write_atomic(const char *path, const uint8_t *data, size_t len) {
  /* Drop any existing file first. ENOENT is fine — first install. */
  unlink(path);

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
  if(fd < 0) return -1;
  size_t off = 0;
  while(off < len) {
    ssize_t w = write(fd, data + off, len - off);
    if(w <= 0) { close(fd); unlink(path); return -1; }
    off += (size_t)w;
  }
  fsync(fd);
  close(fd);
  return 0;
}


static int
update_one_dir(const y2jb_dir_t *d,
               const uint8_t *with_buf, size_t with_len,
               const uint8_t *no_buf,   size_t no_len,
               char *err_out, size_t err_size) {
  int replaced = 0;

  if(d->has_with_etahen) {
    if(!with_buf || with_len == 0) {
      if(err_out) snprintf(err_out, err_size,
          "no elf-arsenal.elf in latest release");
    } else {
      char p[400];
      snprintf(p, sizeof(p), "%s/%s", d->path, ELF_WITH_ETAHEN);
      if(write_atomic(p, with_buf, with_len) == 0) replaced++;
      else if(err_out) snprintf(err_out, err_size,
          "write %s failed: %s", p, strerror(errno));
    }
  }

  if(d->has_no_etahen) {
    if(!no_buf || no_len == 0) {
      if(err_out) snprintf(err_out, err_size,
          "no sonic-loader-no-etahen.elf in latest release");
    } else {
      char p[400];
      snprintf(p, sizeof(p), "%s/%s", d->path, ELF_NO_ETAHEN);
      if(write_atomic(p, no_buf, no_len) == 0) replaced++;
      else if(err_out) snprintf(err_out, err_size,
          "write %s failed: %s", p, strerror(errno));
    }
  }

  return replaced;
}



static enum MHD_Result
serve_json_owned(struct MHD_Connection *conn, unsigned int status,
                 cJSON *root) {
  char *out = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if(!out) return MHD_NO;
  size_t len = strlen(out);
  struct MHD_Response *r =
      MHD_create_response_from_buffer(len, out, MHD_RESPMEM_MUST_FREE);
  if(!r) { free(out); return MHD_NO; }
  MHD_add_response_header(r, "Content-Type", "application/json");
  enum MHD_Result rc = websrv_queue_response(conn, status, r);
  MHD_destroy_response(r);
  return rc;
}


static enum MHD_Result
y2jb_scan_request(struct MHD_Connection *conn) {
  y2jb_dir_t dirs[MAX_DIRS];
  int n = y2jb_scan(dirs);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject  (root, "ok",       1);
  cJSON_AddNumberToObject(root, "found",    n);
  cJSON *arr = cJSON_AddArrayToObject(root, "dirs");
  for(int i = 0; i < n; i++) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "path",            dirs[i].path);
    cJSON_AddBoolToObject  (e, "hasWithEtahen",   dirs[i].has_with_etahen);
    cJSON_AddBoolToObject  (e, "hasNoEtahen",     dirs[i].has_no_etahen);
    cJSON_AddNumberToObject(e, "sizeWithEtahen",  (double)dirs[i].size_with_etahen);
    cJSON_AddNumberToObject(e, "sizeNoEtahen",    (double)dirs[i].size_no_etahen);
    cJSON_AddItemToArray(arr, e);
  }
  return serve_json_owned(conn, MHD_HTTP_OK, root);
}


static enum MHD_Result
y2jb_update_request(struct MHD_Connection *conn) {
  y2jb_dir_t dirs[MAX_DIRS];
  int n = y2jb_scan(dirs);

  if(n == 0) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject  (root, "ok",          0);
    cJSON_AddStringToObject(root, "error",
        "no ps5_autoloader directories found with a sonic-loader ELF");
    cJSON_AddNumberToObject(root, "found",       0);
    cJSON_AddNumberToObject(root, "replaced",    0);
    return serve_json_owned(conn, MHD_HTTP_NOT_FOUND, root);
  }

  int need_with = 0, need_no = 0;
  for(int i = 0; i < n; i++) {
    if(dirs[i].has_with_etahen) need_with = 1;
    if(dirs[i].has_no_etahen)   need_no   = 1;
  }

  latest_release_t rel = {0};
  if(fetch_latest_release(&rel) != 0) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject  (root, "ok",    0);
    cJSON_AddStringToObject(root, "error",
        "could not fetch latest release from git.etawen.dev");
    return serve_json_owned(conn, MHD_HTTP_BAD_GATEWAY, root);
  }

  uint8_t *with_buf = NULL;  size_t with_len = 0;
  uint8_t *no_buf   = NULL;  size_t no_len   = 0;
  if(need_with && rel.with_etahen_url) {
    with_buf = http_get(rel.with_etahen_url, &with_len);
  }
  if(need_no && rel.no_etahen_url) {
    no_buf = http_get(rel.no_etahen_url, &no_len);
  }

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "tag", rel.tag ? rel.tag : "");
  cJSON_AddNumberToObject(root, "found", n);
  cJSON_AddNumberToObject(root, "withEtahenBytes", (double)with_len);
  cJSON_AddNumberToObject(root, "noEtahenBytes",   (double)no_len);

  cJSON *arr = cJSON_AddArrayToObject(root, "results");
  int total_replaced = 0, total_errors = 0;
  for(int i = 0; i < n; i++) {
    char err[256] = "";
    int replaced = update_one_dir(&dirs[i],
                                  with_buf, with_len,
                                  no_buf,   no_len,
                                  err, sizeof(err));
    total_replaced += replaced;
    if(err[0]) total_errors++;
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "path",     dirs[i].path);
    cJSON_AddNumberToObject(e, "replaced", replaced);
    if(err[0]) cJSON_AddStringToObject(e, "error", err);
    cJSON_AddItemToArray(arr, e);
  }
  cJSON_AddNumberToObject(root, "replaced", total_replaced);
  cJSON_AddNumberToObject(root, "errors",   total_errors);
  cJSON_AddBoolToObject  (root, "ok",       total_errors == 0 && total_replaced > 0);

  free(with_buf);
  free(no_buf);
  release_free(&rel);

  return serve_json_owned(conn, MHD_HTTP_OK, root);
}



#include <pthread.h>

#define STARTUP_DELAY_SECONDS  30


static void
y2jb_silent_verify(void) {
  y2jb_dir_t dirs[MAX_DIRS];
  int n = y2jb_scan(dirs);
  if(n == 0) {
    fprintf(stderr, "y2jb: silent verify — no autoloader dirs, nothing to do\n");
    return;
  }

  latest_release_t rel = {0};
  if(fetch_latest_release(&rel) != 0) {
    fprintf(stderr, "y2jb: silent verify — could not fetch latest release\n");
    return;
  }

  int need_with_dl = 0, need_no_dl = 0;
  int stale_with[MAX_DIRS] = {0};
  int stale_no[MAX_DIRS]   = {0};
  for(int i = 0; i < n; i++) {
    if(dirs[i].has_with_etahen) {
      if(rel.size_with_etahen <= 0 ||
         dirs[i].size_with_etahen != rel.size_with_etahen) {
        stale_with[i] = 1;
        need_with_dl  = 1;
      }
    }
    if(dirs[i].has_no_etahen) {
      if(rel.size_no_etahen <= 0 ||
         dirs[i].size_no_etahen != rel.size_no_etahen) {
        stale_no[i] = 1;
        need_no_dl  = 1;
      }
    }
  }

  if(!need_with_dl && !need_no_dl) {
    fprintf(stderr, "y2jb: silent verify — all %d dir(s) match release %s, nothing to update\n",
            n, rel.tag ? rel.tag : "?");
    release_free(&rel);
    return;
  }

  uint8_t *with_buf = NULL;  size_t with_len = 0;
  uint8_t *no_buf   = NULL;  size_t no_len   = 0;
  if(need_with_dl && rel.with_etahen_url) {
    with_buf = http_get(rel.with_etahen_url, &with_len);
  }
  if(need_no_dl && rel.no_etahen_url) {
    no_buf = http_get(rel.no_etahen_url, &no_len);
  }

  int total_replaced = 0;
  for(int i = 0; i < n; i++) {
    const uint8_t *w = stale_with[i] ? with_buf : NULL;
    size_t         wl = stale_with[i] ? with_len : 0;
    const uint8_t *o = stale_no[i]   ? no_buf   : NULL;
    size_t         ol = stale_no[i]   ? no_len   : 0;
    if(!w && !o) continue;
    char err[256] = "";
    int replaced = update_one_dir(&dirs[i], w, wl, o, ol, err, sizeof(err));
    total_replaced += replaced;
    if(err[0]) {
      fprintf(stderr, "y2jb: silent verify — %s: %s\n", dirs[i].path, err);
    } else if(replaced > 0) {
      fprintf(stderr, "y2jb: silent verify — refreshed %d file(s) in %s to %s\n",
              replaced, dirs[i].path, rel.tag ? rel.tag : "latest");
    }
  }

  fprintf(stderr, "y2jb: silent verify — %d/%d dir(s) refreshed to %s\n",
          total_replaced, n, rel.tag ? rel.tag : "latest");

  free(with_buf);
  free(no_buf);
  release_free(&rel);
}



#define PLDMGR_PAYLOADS_DIR "/data/pldmgr/payloads"
#define PLDMGR_WITH_ETAHEN  PLDMGR_PAYLOADS_DIR "/sonic-loader"
#define PLDMGR_NO_ETAHEN    PLDMGR_PAYLOADS_DIR "/sonic-loader-no-etahen"

/* If either pldmgr payload file exists and its size differs from the
   latest release, download and replace it. Silent on network failure. */
static void
pldmgr_silent_update(void) {
  int has_with = (file_size(PLDMGR_WITH_ETAHEN) > 4);
  int has_no   = (file_size(PLDMGR_NO_ETAHEN)   > 4);
  if(!has_with && !has_no) return;

  latest_release_t rel = {0};
  if(fetch_latest_release(&rel) != 0) {
    fprintf(stderr, "pldmgr: could not fetch latest release\n");
    return;
  }

  int need_with = has_with && (rel.size_with_etahen <= 0 ||
      file_size(PLDMGR_WITH_ETAHEN) != rel.size_with_etahen);
  int need_no   = has_no   && (rel.size_no_etahen   <= 0 ||
      file_size(PLDMGR_NO_ETAHEN)   != rel.size_no_etahen);

  if(!need_with && !need_no) {
    fprintf(stderr, "pldmgr: already at %s\n", rel.tag ? rel.tag : "?");
    release_free(&rel);
    return;
  }

  if(need_with && rel.with_etahen_url) {
    size_t len = 0;
    uint8_t *buf = http_get(rel.with_etahen_url, &len);
    if(buf && len > 0) {
      if(write_atomic(PLDMGR_WITH_ETAHEN, buf, len) == 0)
        fprintf(stderr, "pldmgr: updated sonic-loader to %s\n",
                rel.tag ? rel.tag : "latest");
      else
        fprintf(stderr, "pldmgr: write %s failed: %s\n",
                PLDMGR_WITH_ETAHEN, strerror(errno));
    }
    free(buf);
  }

  if(need_no && rel.no_etahen_url) {
    size_t len = 0;
    uint8_t *buf = http_get(rel.no_etahen_url, &len);
    if(buf && len > 0) {
      if(write_atomic(PLDMGR_NO_ETAHEN, buf, len) == 0)
        fprintf(stderr, "pldmgr: updated sonic-loader-no-etahen to %s\n",
                rel.tag ? rel.tag : "latest");
      else
        fprintf(stderr, "pldmgr: write %s failed: %s\n",
                PLDMGR_NO_ETAHEN, strerror(errno));
    }
    free(buf);
  }

  release_free(&rel);
}


static void *
y2jb_startup_thread(void *arg) {
  (void)arg;
  syscall(SYS_thr_set_name, -1, "y2jb-verify");
  sleep(STARTUP_DELAY_SECONDS);
  y2jb_silent_verify();
  pldmgr_silent_update();
  return NULL;
}


void
y2jb_startup_init(void) {
  pthread_t t;
  if(pthread_create(&t, NULL, y2jb_startup_thread, NULL) != 0) {
    perror("y2jb_startup_init: pthread_create");
    return;
  }
  pthread_detach(t);
}


enum MHD_Result
y2jb_request(struct MHD_Connection *conn, const char *url) {
  if(!strcmp(url, "/api/y2jb"))         return y2jb_scan_request(conn);
  if(!strcmp(url, "/api/y2jb/scan"))    return y2jb_scan_request(conn);
  if(!strcmp(url, "/api/y2jb/update"))  return y2jb_update_request(conn);

  const char *err = "{\"ok\":false,\"error\":\"no such endpoint\"}";
  struct MHD_Response *r =
      MHD_create_response_from_buffer(strlen(err), (void*)err,
                                      MHD_RESPMEM_PERSISTENT);
  MHD_add_response_header(r, "Content-Type", "application/json");
  enum MHD_Result rc = websrv_queue_response(conn, MHD_HTTP_NOT_FOUND, r);
  MHD_destroy_response(r);
  return rc;
}
