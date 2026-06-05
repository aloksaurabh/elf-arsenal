
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <microhttpd.h>

#include "linux_loader.h"
#include "offline_pack.h"
#include "sys.h"
#include "ps5/http.h"
#include "third_party/cJSON.h"
#include "websrv.h"

#define LINUX_DIR        "/data/elf-arsenal/dl"
#define LINUX_ELF_PATH   LINUX_DIR "/ps5-linux-loader.elf"
#define LINUX_VER_PATH   LINUX_DIR "/ps5-linux-loader.version"
#define LINUX_API_LATEST \
  "https://api.github.com/repos/ps5-linux/ps5-linux-loader/releases/latest"
#define LINUX_API_TAG_PREFIX \
  "https://api.github.com/repos/ps5-linux/ps5-linux-loader/releases/tags/"

#define LINUX_FW_MAX     0x0602ffffu

extern unsigned char ps5_linux_loader_elf[];
extern unsigned int  ps5_linux_loader_elf_size;
#define LINUX_BUNDLED_TAG  "v2.1"

static atomic_int g_dl_in_progress  = 0;
static atomic_int g_dl_last_status  = 0;   /* 0=idle, 1=ok, -1=error */
static char       g_dl_last_error[256] = {0};
static char       g_installed_tag[32]   = {0};


int
linux_loader_supported(void) {
  unsigned int fw = sys_get_firmware_version();
  if(fw == 0) return 0;
  return (fw <= LINUX_FW_MAX) ? 1 : 0;
}


int
linux_loader_is_installed(void) {
  struct stat st;
  if(stat(LINUX_ELF_PATH, &st) != 0) return 0;
  return st.st_size > 0 ? 1 : 0;
}


static char*
parse_release_for_asset(const uint8_t *body, size_t len, char *tag_out,
                        size_t tag_out_size) {
  char *txt = malloc(len + 1);
  if(!txt) return NULL;
  memcpy(txt, body, len);
  txt[len] = 0;
  cJSON *root = cJSON_Parse(txt);
  free(txt);
  if(!root) return NULL;

  cJSON *tag = cJSON_GetObjectItem(root, "tag_name");
  if(cJSON_IsString(tag) && tag->valuestring && tag_out) {
    strncpy(tag_out, tag->valuestring, tag_out_size - 1);
    tag_out[tag_out_size - 1] = 0;
  }

  char *url = NULL;
  cJSON *assets = cJSON_GetObjectItem(root, "assets");
  if(cJSON_IsArray(assets)) {
    cJSON *a;
    cJSON_ArrayForEach(a, assets) {
      cJSON *name = cJSON_GetObjectItem(a, "name");
      cJSON *dlu  = cJSON_GetObjectItem(a, "browser_download_url");
      if(!cJSON_IsString(name) || !cJSON_IsString(dlu)) continue;
      if(!strcasecmp(name->valuestring, "ps5-linux-loader.elf")) {
        url = strdup(dlu->valuestring);
        break;
      }
    }
  }
  cJSON_Delete(root);
  return url;
}


static int
do_download_tag(const char *want_tag) {
  mkdir(LINUX_DIR, 0755);

  /* 1. release metadata. */
  char api_url[512];
  if(want_tag && want_tag[0]) {
    /* Caller asked for a specific tag — hit /releases/tags/<tag>. */
    snprintf(api_url, sizeof(api_url), "%s%s",
             LINUX_API_TAG_PREFIX, want_tag);
  } else {
    snprintf(api_url, sizeof(api_url), "%s", LINUX_API_LATEST);
  }
  size_t mlen = 0;
  uint8_t *meta = http_get(api_url, &mlen);
  if(!meta || mlen == 0) {
    free(meta);
    snprintf(g_dl_last_error, sizeof(g_dl_last_error),
             "could not fetch %s release metadata "
             "(GitHub API — network / TLS / rate-limit)",
             (want_tag && want_tag[0]) ? want_tag : "latest");
    return -1;
  }

  char tag[32] = {0};
  char *asset_url = parse_release_for_asset(meta, mlen, tag, sizeof(tag));
  free(meta);
  if(!asset_url) {
    snprintf(g_dl_last_error, sizeof(g_dl_last_error),
             "release metadata didn't include "
             "a 'ps5-linux-loader.elf' asset (upstream layout changed?)");
    return -1;
  }

  size_t elen = 0;
  uint8_t *elf = http_get(asset_url, &elen);
  free(asset_url);
  if(!elf || elen == 0) {
    free(elf);
    snprintf(g_dl_last_error, sizeof(g_dl_last_error),
             "release asset download failed (302 not followed? TLS?)");
    return -1;
  }
  if(elen < 1024) {
    free(elf);
    snprintf(g_dl_last_error, sizeof(g_dl_last_error),
             "release asset is implausibly small (%zu bytes) — probably "
             "an error page rather than the loader ELF", elen);
    return -1;
  }

  /* 3. atomic write: .tmp -> rename. */
  char tmp[160];
  snprintf(tmp, sizeof(tmp), "%s.tmp", LINUX_ELF_PATH);
  int fd = open(tmp, O_WRONLY|O_CREAT|O_TRUNC, 0644);
  if(fd < 0) {
    free(elf);
    snprintf(g_dl_last_error, sizeof(g_dl_last_error),
             "couldn't open %s for writing: %s", tmp, strerror(errno));
    return -1;
  }
  ssize_t wrote = write(fd, elf, elen);
  close(fd);
  free(elf);
  if(wrote != (ssize_t)elen) {
    unlink(tmp);
    snprintf(g_dl_last_error, sizeof(g_dl_last_error),
             "short write to %s (%zd of %zu bytes)",
             tmp, wrote, elen);
    return -1;
  }
  if(rename(tmp, LINUX_ELF_PATH) != 0) {
    unlink(tmp);
    snprintf(g_dl_last_error, sizeof(g_dl_last_error),
             "rename %s -> %s failed: %s", tmp, LINUX_ELF_PATH, strerror(errno));
    return -1;
  }

  /* 4. write companion version file so the UI can show what's cached. */
  if(tag[0]) {
    FILE *vf = fopen(LINUX_VER_PATH, "w");
    if(vf) {
      fprintf(vf, "%s\n", tag);
      fclose(vf);
      strncpy(g_installed_tag, tag, sizeof(g_installed_tag)-1);
      g_installed_tag[sizeof(g_installed_tag)-1] = 0;
    }
  }
  return 0;
}


static void*
dl_thread_fn(void *arg) {
  /* If a tag was supplied, the kick-off path strdup'd it and passes
     ownership to us. NULL means "latest". */
  char *want_tag = (char*)arg;
  int rc = do_download_tag(want_tag);
  atomic_store(&g_dl_last_status, rc == 0 ? 1 : -1);
  atomic_store(&g_dl_in_progress, 0);
  if(rc == 0) {
    printf("ps5-linux-loader: cached at %s (tag %s)\n",
           LINUX_ELF_PATH,
           g_installed_tag[0] ? g_installed_tag : "?");
  } else {
    fprintf(stderr, "ps5-linux-loader: %s\n", g_dl_last_error);
  }
  free(want_tag);
  return NULL;
}


static int
kick_download(const char *want_tag) {
  int expected = 0;
  if(!atomic_compare_exchange_strong(&g_dl_in_progress, &expected, 1)) {
    return 0;
  }
  g_dl_last_error[0] = 0;
  atomic_store(&g_dl_last_status, 0);
  char *tag_arg = (want_tag && want_tag[0]) ? strdup(want_tag) : NULL;
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  pthread_create(&t, &a, dl_thread_fn, tag_arg);
  pthread_attr_destroy(&a);
  return 1;
}


static int
install_bundled_linux_loader(void) {
  mkdir(LINUX_DIR, 0755);

  /* The bundled loader is embedded gzip-compressed — inflate it first. */
  size_t loader_size = 0;
  uint8_t *loader = sys_gz_inflate(ps5_linux_loader_elf,
                                   ps5_linux_loader_elf_size, &loader_size);
  if(!loader) {
    fprintf(stderr, "ps5-linux-loader: install_bundled: inflate failed\n");
    return -1;
  }

  char tmp[160];
  snprintf(tmp, sizeof(tmp), "%s.tmp", LINUX_ELF_PATH);
  int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0755);
  if(fd < 0) {
    fprintf(stderr,
            "ps5-linux-loader: install_bundled: open %s failed: %s\n",
            tmp, strerror(errno));
    free(loader);
    return -1;
  }
  size_t off = 0;
  while(off < loader_size) {
    ssize_t w = write(fd, loader + off, loader_size - off);
    if(w <= 0) {
      close(fd);
      unlink(tmp);
      free(loader);
      fprintf(stderr,
              "ps5-linux-loader: install_bundled: write failed: %s\n",
              strerror(errno));
      return -1;
    }
    off += (size_t)w;
  }
  fsync(fd);
  close(fd);
  free(loader);
  if(rename(tmp, LINUX_ELF_PATH) != 0) {
    unlink(tmp);
    fprintf(stderr,
            "ps5-linux-loader: install_bundled: rename failed: %s\n",
            strerror(errno));
    return -1;
  }

  FILE *vf = fopen(LINUX_VER_PATH, "w");
  if(vf) {
    fprintf(vf, "%s\n", LINUX_BUNDLED_TAG);
    fclose(vf);
  }
  strncpy(g_installed_tag, LINUX_BUNDLED_TAG, sizeof(g_installed_tag)-1);
  g_installed_tag[sizeof(g_installed_tag)-1] = 0;
  atomic_store(&g_dl_last_status, 1);

  fprintf(stderr, "ps5-linux-loader: installed bundled %s (%u bytes)\n",
          LINUX_BUNDLED_TAG, ps5_linux_loader_elf_size);
  return 0;
}


void
linux_loader_init(void) {
  if(!linux_loader_supported()) return;

  /* Load cached version tag if present so /api/linux/status can show
     what's on disk before any fresh install runs. */
  FILE *vf = fopen(LINUX_VER_PATH, "r");
  if(vf) {
    if(fgets(g_installed_tag, sizeof(g_installed_tag), vf)) {
      size_t L = strlen(g_installed_tag);
      while(L > 0 && (g_installed_tag[L-1] == '\n' ||
                      g_installed_tag[L-1] == '\r' ||
                      g_installed_tag[L-1] == ' ')) {
        g_installed_tag[--L] = 0;
      }
    }
    fclose(vf);
  }

  if(!linux_loader_is_installed()) {
    install_bundled_linux_loader();
  }
}



static enum MHD_Result
serve_json(struct MHD_Connection *conn, int code, cJSON *r) {
  char *s = cJSON_PrintUnformatted(r);
  size_t len = s ? strlen(s) : 0;
  struct MHD_Response *resp = MHD_create_response_from_buffer(
      len, s, MHD_RESPMEM_MUST_FREE);
  if(!resp) { free(s); return MHD_NO; }
  MHD_add_response_header(resp, "Content-Type", "application/json");
  MHD_add_response_header(resp, "Cache-Control", "no-cache");
  enum MHD_Result rc = websrv_queue_response(conn, code, resp);
  MHD_destroy_response(resp);
  return rc;
}


enum MHD_Result
linux_loader_request(struct MHD_Connection *conn, const char *url,
                     const char *method) {
  (void)method;

  if(!strcmp(url, "/api/linux/status")) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "supported", linux_loader_supported());
    cJSON_AddBoolToObject(r, "installed", linux_loader_is_installed());
    cJSON_AddBoolToObject(r, "downloadInProgress",
                          atomic_load(&g_dl_in_progress));
    int s = atomic_load(&g_dl_last_status);
    cJSON_AddStringToObject(r, "lastStatus",
                            s == 1 ? "ok" : s == -1 ? "error" : "idle");
    if(g_dl_last_error[0]) {
      cJSON_AddStringToObject(r, "error", g_dl_last_error);
    }
    if(g_installed_tag[0]) {
      cJSON_AddStringToObject(r, "version", g_installed_tag);
    }
    cJSON_AddStringToObject(r, "path", LINUX_ELF_PATH);
    struct stat st;
    if(stat(LINUX_ELF_PATH, &st) == 0) {
      cJSON_AddNumberToObject(r, "sizeBytes", (double)st.st_size);
    }
    unsigned int fw = sys_get_firmware_version();
    char fwstr[16];
    snprintf(fwstr, sizeof(fwstr), "%02x.%02x",
             (fw >> 24) & 0xff, (fw >> 16) & 0xff);
    cJSON_AddStringToObject(r, "firmware", fwstr);
    enum MHD_Result rc = serve_json(conn, MHD_HTTP_OK, r);
    cJSON_Delete(r);
    return rc;
  }

  if(!strcmp(url, "/api/linux/download")) {
    if(!linux_loader_supported()) {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddBoolToObject(r, "ok", 0);
      cJSON_AddStringToObject(r, "error",
          "firmware does not support the PS5 Linux loader "
          "(supported range: 1.00 — 6.02)");
      enum MHD_Result rc = serve_json(conn, MHD_HTTP_BAD_REQUEST, r);
      cJSON_Delete(r);
      return rc;
    }
    const char *want_tag = MHD_lookup_connection_value(
        conn, MHD_GET_ARGUMENT_KIND, "tag");
    int started = kick_download(want_tag);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 1);
    cJSON_AddBoolToObject(r, "started", started);
    if(want_tag && want_tag[0]) {
      cJSON_AddStringToObject(r, "tag", want_tag);
    }
    cJSON_AddStringToObject(r, "message",
        started ? "download started — poll /api/linux/status for progress"
                : "a download is already in progress");
    enum MHD_Result rc = serve_json(conn, MHD_HTTP_OK, r);
    cJSON_Delete(r);
    return rc;
  }

  if(!strcmp(url, "/api/linux/launch")) {
    if(!linux_loader_supported()) {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddBoolToObject(r, "ok", 0);
      cJSON_AddStringToObject(r, "error", "firmware not supported");
      enum MHD_Result rc = serve_json(conn, MHD_HTTP_BAD_REQUEST, r);
      cJSON_Delete(r);
      return rc;
    }
    if(!linux_loader_is_installed()) {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddBoolToObject(r, "ok", 0);
      cJSON_AddStringToObject(r, "error",
          "loader ELF not on disk yet — open Settings → 🐧 PS5 Linux "
          "and tap Download / Update first");
      enum MHD_Result rc = serve_json(conn, MHD_HTTP_BAD_REQUEST, r);
      cJSON_Delete(r);
      return rc;
    }

    /* sys_launch_daemon accepts a file:// or http:// or bare path URI.
       For a local file we pass "file://" + absolute path. */
    char uri[160];
    snprintf(uri, sizeof(uri), "file://%s", LINUX_ELF_PATH);
    int fd = sys_launch_daemon("/", uri, "", "");
    if(fd < 0) {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddBoolToObject(r, "ok", 0);
      cJSON_AddStringToObject(r, "error", "launch failed (elfldr_spawn)");
      enum MHD_Result rc = serve_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, r);
      cJSON_Delete(r);
      return rc;
    }
    /* The daemon path returns a pipe FD for stdout streaming; we don't
       need to forward it here, just close. */
    close(fd);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 1);
    cJSON_AddStringToObject(r, "message",
        "ps5-linux-loader launched — Linux should take over the display");
    enum MHD_Result rc = serve_json(conn, MHD_HTTP_OK, r);
    cJSON_Delete(r);
    return rc;
  }

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", 0);
  cJSON_AddStringToObject(r, "error", "unknown /api/linux endpoint");
  enum MHD_Result rc = serve_json(conn, MHD_HTTP_NOT_FOUND, r);
  cJSON_Delete(r);
  return rc;
}
