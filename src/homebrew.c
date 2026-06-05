
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <microhttpd.h>

#include "zipread.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <pthread.h>

#include "backup.h"
#include "homebrew.h"
#include "ps5/notify.h"
#include "jb.h"
#include "ps5/http.h"
#include "sys.h"
#include "third_party/cJSON.h"
#include "websrv.h"


#define WEBSRV_RELEASE_BASE \
  "https://github.com/ps5-payload-dev/websrv/releases/latest/download/"

#define LAUNCHER_PKG_URL \
  "https://git.etawen.dev/soniciso/elf-arsenal/raw/branch/main/" \
  "payloads/elf-arsenal-tile.pkg"

#define DOWNLOAD_DIR  "/data/elf-arsenal/dl"
#define HOMEBREW_DIR  "/data/homebrew"
#define PKG_DIR       "/data/elf-arsenal/pkgs"
#define PKG_UPLOAD_DIR PKG_DIR "/uploaded"


struct hb_asset { const char *file; const char *display; };
static const struct hb_asset g_assets[] = {
  {"DevilutionX.zip",   "DevilutionX (Diablo)"},
  {"EDuke32.zip",       "EDuke32 (Duke 3D engine)"},
  {"FBNeo.zip",         "FBNeo (Final Burn Neo)"},
  {"FFplay.zip",        "FFplay"},
  {"LakeSnes.zip",      "LakeSnes (SNES)"},
  {"LinkDev.zip",       "LinkDev"},
  {"Mednafen.zip",      "Mednafen multi-system"},
  {"OffAct.zip",        "OffAct"},
  {"Omnispeak.zip",     "Omnispeak (Commander Keen)"},
  {"OpenJazz.zip",      "OpenJazz"},
  {"PKGInstall.zip",    "PKG Install (RemotePKG-style)"},
  {"RetroArch.zip",     "RetroArch"},
  {"ScummVM.zip",       "ScummVM"},
  {"SverigesRadio.zip", "Sveriges Radio"},
  {"SVTplay.zip",       "SVT Play"},
  {"Transmission.zip",  "Transmission (BitTorrent)"},
  {"TVHeadend.zip",     "TVHeadend"},
  {"YQuake2.zip",       "YQuake2"},
  {NULL, NULL}
};


/*  Helpers                                                               */

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
  if(!txt) {
    return serve_buffer(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                        "application/json", "{\"error\":\"alloc\"}", 17, 0);
  }
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
is_safe_asset_name(const char *s) {
  if(!s || !*s) return 0;
  size_t n = strlen(s);
  if(n > 64) return 0;
  for(size_t i=0; i<n; i++) {
    char c = s[i];
    if(!isalnum((unsigned char)c) && c != '.' && c != '-' && c != '_') {
      return 0;
    }
  }
  if(strstr(s, "..")) return 0;
  return 1;
}


static int
ensure_dir(const char *path) {
  struct stat st;
  if(stat(path, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
  return mkdir(path, 0755);
}


static int
write_file_bytes(const char *path, const void *data, size_t len) {
  int fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, 0644);
  if(fd < 0) return -1;
  ssize_t n = write(fd, data, len);
  close(fd);
  return (n == (ssize_t)len) ? 0 : -1;
}


/*  ZIP extraction (libarchive)                                           */

static int
mkpath_recursive(const char *path) {
  /* mkdir -p */
  char tmp[1024];
  size_t len = strnlen(path, sizeof(tmp));
  if(len >= sizeof(tmp)) return -1;
  memcpy(tmp, path, len + 1);
  for(char *p = tmp + 1; *p; p++) {
    if(*p == '/') {
      *p = 0;
      mkdir(tmp, 0755);
      *p = '/';
    }
  }
  return mkdir(tmp, 0755);
}


static int
extract_zip_to_homebrew(const uint8_t *data, size_t len, char *err,
                        size_t err_size) {
  ezip *a = ezip_open_mem(data, len);
  if(ezip_failed(a)) {
    snprintf(err, err_size, "archive open: %s", ezip_error(a));
    ezip_close(a);
    return -1;
  }

  ensure_dir("/data");
  ensure_dir(HOMEBREW_DIR);

  int file_count = 0;
  ezip_entry entry;
  int rc;

  while((rc = ezip_next(a, &entry)) == 1) {
    const char *name = entry.path;
    if(!name || !*name) continue;
    if(strstr(name, "..")) continue; /* path traversal guard */

    char dest[1024];
    if(snprintf(dest, sizeof(dest), HOMEBREW_DIR "/%s", name) >= (int)sizeof(dest)) {
      continue;
    }

    if(entry.is_dir) {
      mkpath_recursive(dest);
      continue;
    }

    /* Make parent dir. */
    char parent[1024];
    snprintf(parent, sizeof(parent), "%s", dest);
    char *slash = strrchr(parent, '/');
    if(slash) {
      *slash = 0;
      mkpath_recursive(parent);
    }

    int fd = open(dest, O_WRONLY|O_CREAT|O_TRUNC, 0755);
    if(fd < 0) {
      snprintf(err, err_size, "open %s: %s", dest, strerror(errno));
      ezip_close(a);
      return -1;
    }

    ezip_stream *s = ezip_stream_open(a);
    if(!s) {
      close(fd); unlink(dest);
      snprintf(err, err_size, "extract: %s", ezip_error(a));
      ezip_close(a);
      return -1;
    }
    uint8_t buf[16 * 1024];
    ssize_t n;
    int ok = 1;
    while((n = ezip_stream_read(s, buf, sizeof(buf))) > 0) {
      if(write(fd, buf, (size_t)n) != n) {
        snprintf(err, err_size, "write %s: %s", dest, strerror(errno));
        ok = 0; break;
      }
    }
    ezip_stream_close(s);
    close(fd);
    if(!ok) { unlink(dest); ezip_close(a); return -1; }
    file_count++;
  }

  ezip_close(a);
  if(rc < 0) {
    snprintf(err, err_size, "iter: ezip error");
    return -1;
  }
  return file_count;
}


/*  HTTP handlers                                                         */

static enum MHD_Result
list_assets(struct MHD_Connection *conn) {
  cJSON *r = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(r, "assets");
  for(const struct hb_asset *a = g_assets; a->file; a++) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "file",  a->file);
    cJSON_AddStringToObject(e, "name",  a->display);
    char url[256];
    snprintf(url, sizeof(url), "%s%s", WEBSRV_RELEASE_BASE, a->file);
    cJSON_AddStringToObject(e, "url",   url);
    cJSON_AddItemToArray(arr, e);
  }
  cJSON_AddStringToObject(r, "destination", HOMEBREW_DIR);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


static enum MHD_Result
install_asset(struct MHD_Connection *conn) {
  const char *asset = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "asset");
  if(!asset || !is_safe_asset_name(asset)) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "bad asset name");
  }
  /* Only allow assets we know about. */
  const struct hb_asset *match = NULL;
  for(const struct hb_asset *a = g_assets; a->file; a++) {
    if(!strcmp(a->file, asset)) { match = a; break; }
  }
  if(!match) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "unknown asset");
  }

  char url[256];
  snprintf(url, sizeof(url), "%s%s", WEBSRV_RELEASE_BASE, asset);
  size_t blen = 0;
  uint8_t *body = http_get(url, &blen);
  if(!body || blen == 0) {
    return serve_error(conn, MHD_HTTP_BAD_GATEWAY, "download failed");
  }

  char err[256] = {0};
  int file_count = extract_zip_to_homebrew(body, blen, err, sizeof(err));
  free(body);
  if(file_count < 0) {
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                       err[0] ? err : "extract failed");
  }

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", 1);
  cJSON_AddStringToObject(r, "asset", asset);
  cJSON_AddNumberToObject(r, "files", file_count);
  cJSON_AddNumberToObject(r, "size",  (double)blen);
  cJSON_AddStringToObject(r, "destination", HOMEBREW_DIR);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


/*  Homebrew launcher PKG install                                         */

/* Struct definitions used by Elf Arsenal's PKG installer flow.
   — these are the exact ABI shapes sceAppInstUtilInstallByPackage expects. */

#define HB_PLAYGOSCENARIOID_SIZE 3
#define HB_CONTENTID_SIZE        0x30
#define HB_LANGUAGE_SIZE         8

typedef char hb_playgo_scenario_id_t[HB_PLAYGOSCENARIOID_SIZE];
typedef char hb_language_t[HB_LANGUAGE_SIZE];
typedef char hb_content_id_t[HB_CONTENTID_SIZE];

typedef struct {
  hb_content_id_t content_id;
  int             content_type;
  int             content_platform;
} hb_pkg_info_t;

typedef struct {
  const char *uri;
  const char *ex_uri;
  const char *playgo_scenario_id;
  const char *content_id;
  const char *content_name;
  const char *icon_url;
} hb_meta_info_t;

#define HB_NUM_LANGUAGES 30
#define HB_NUM_IDS       64

typedef struct {
  hb_language_t            languages[HB_NUM_LANGUAGES];
  hb_playgo_scenario_id_t  playgo_scenario_ids[HB_NUM_IDS];
  hb_content_id_t          content_ids[HB_NUM_IDS];
  long                     unknown[810];
} hb_playgo_info_t;

extern int sceAppInstUtilInitialize(void);
extern int sceAppInstUtilInstallByPackage(hb_meta_info_t *meta,
                                          hb_pkg_info_t  *pkg_info,
                                          hb_playgo_info_t *play);
extern int sceAppInstUtilAppInstallPkg(const char *path,
                                       hb_pkg_info_t *pkg_info);




static int
read_pkg_content_id(const char *path, char *out, size_t out_size) {
  if(out_size < 37) return -1;
  int fd = open(path, O_RDONLY);
  if(fd < 0) return -1;
  unsigned char hdr[0x80];
  ssize_t n = read(fd, hdr, sizeof(hdr));
  close(fd);
  if(n < (ssize_t)sizeof(hdr)) return -1;

  if(hdr[0] != 0x7F) return -1;

  for(int i = 0; i < 36; i++) {
    unsigned char c = hdr[0x40 + i];
    if(c == 0) break;             /* NUL pad — end of ContentID */
    if(c < 0x20 || c > 0x7E) return -1;  /* not printable */
    out[i] = (char)c;
  }
  out[36] = 0;
  /* Trim trailing whitespace just in case. */
  for(int i = (int)strlen(out) - 1; i >= 0 && out[i] == ' '; i--) out[i] = 0;
  return out[0] ? 0 : -1;
}


static int
canonicalise_pkg_filename(char *path, size_t path_size) {
  char cid[40];
  if(read_pkg_content_id(path, cid, sizeof(cid)) != 0) return -1;

  /* Find the directory part. */
  const char *slash = strrchr(path, '/');
  size_t dir_len = slash ? (size_t)(slash - path) : 0;
  /* Already-correct filename → no rename. */
  const char *base = slash ? slash + 1 : path;
  char want[64];
  snprintf(want, sizeof(want), "%s.pkg", cid);
  if(strcmp(base, want) == 0) return 0;

  char new_path[512];
  if(dir_len > 0) {
    if(dir_len + 1 + strlen(want) >= sizeof(new_path)) return -1;
    memcpy(new_path, path, dir_len);
    new_path[dir_len] = '/';
    strcpy(new_path + dir_len + 1, want);
  } else {
    strcpy(new_path, want);
  }
  if(rename(path, new_path) != 0) return -1;

  /* Surface the new path to the caller. */
  if(strlen(new_path) + 1 > path_size) return -1;
  strcpy(path, new_path);
  return 0;
}


static void
rewrite_path_for_install(const char *in, char *out, size_t out_size) {
  if(!in || !out || out_size == 0) return;
  if(strncmp(in, "/data/", 6) == 0) {
    /* /data/foo  ->  /user/data/foo */
    snprintf(out, out_size, "/user%s", in);
    return;
  }
  /* Anything else (URL, /user/data/..., /mnt/usb...) is passed through. */
  strncpy(out, in, out_size - 1);
  out[out_size - 1] = '\0';
}


static int
dpi_send_install(const char *url_or_path, char *resp_out, size_t resp_size) {
  /* Skip DPI v1 entirely when v2 is the active install path. */
  if(sys_dpiv2_get_enabled()) return -1;
  /* Make sure DPI is up first — boot-time spawn might have lost the
     race or DPI could have crashed/been killed. */
  (void)sys_dpi_ensure_running();

  /* Rewrite /data/... → /user/data/... so the install service in its
     sandbox can actually see the file. */
  char fixed[1024];
  rewrite_path_for_install(url_or_path, fixed, sizeof(fixed));

  int s = socket(AF_INET, SOCK_STREAM, 0);
  if(s < 0) return -1;

  int yes = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

  struct sockaddr_in sa = {0};
  sa.sin_family      = AF_INET;
  sa.sin_port        = htons(9040);
  sa.sin_addr.s_addr = inet_addr("127.0.0.1");
  if(connect(s, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
    close(s);
    return -1;
  }

  size_t len = strlen(fixed);
  ssize_t sent = send(s, fixed, len, 0);
  if(sent < 0 || (size_t)sent != len) { close(s); return -1; }

  if(resp_out && resp_size > 0) {
    ssize_t n = recv(s, resp_out, resp_size - 1, 0);
    if(n < 0) n = 0;
    resp_out[n] = '\0';
  }
  close(s);
  return 0;
}


static int
install_pkg_at_path(const char *path, const char *content_name,
                    char *content_id_out, size_t content_id_out_size,
                    int *which_path_out) {
  /* DPI-first path. */
  char dpi_resp[256] = {0};
  if(dpi_send_install(path, dpi_resp, sizeof(dpi_resp)) == 0) {
    if(which_path_out) *which_path_out = 1;
    int dpi_ok = (strncasecmp(dpi_resp, "ok", 2) == 0 ||
                  strncasecmp(dpi_resp, "queued", 6) == 0 ||
                  dpi_resp[0] == '\0');  /* empty = silent accept */
    if(dpi_ok) {
      /* Best-effort content-id read from the PKG header so the JSON
         response keeps the contentId field for the UI. */
      if(content_id_out && content_id_out_size > 0 &&
         path[0] == '/' /* skip URL inputs — no local read possible */) {
        char cid[40] = {0};
        if(read_pkg_content_id(path, cid, sizeof(cid)) == 0) {
          strncpy(content_id_out, cid, content_id_out_size - 1);
          content_id_out[content_id_out_size - 1] = '\0';
        }
      }
      return 0;
    }
  }

  /* DPI unreachable — try InstallByPackage directly in Arsenal's process.
     Arsenal already holds system credentials so sceAppInstUtilInitialize
     succeeds here even on FW 11+.  Initialize exactly once — repeated
     calls appear to silently reset IPMI state, causing silent failures. */
  static pthread_mutex_t ai_mtx  = PTHREAD_MUTEX_INITIALIZER;
  static int             ai_done = 0;
  pthread_mutex_lock(&ai_mtx);
  if (!ai_done) { sceAppInstUtilInitialize(); ai_done = 1; }
  pthread_mutex_unlock(&ai_mtx);

  if(strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0) {
    hb_meta_info_t umeta = {0};
    umeta.uri                = path;
    umeta.ex_uri             = "";
    umeta.playgo_scenario_id = "";
    umeta.content_id         = "";
    umeta.content_name       = content_name ? content_name : "Elf Arsenal";
    umeta.icon_url           = "";
    hb_pkg_info_t    upkg    = {0};
    hb_playgo_info_t uplaygo = {0};
    int urc = sceAppInstUtilInstallByPackage(&umeta, &upkg, &uplaygo);
    if(which_path_out) *which_path_out = (urc == 0) ? 4 : 0;
    if(urc == 0 && content_id_out && content_id_out_size > 0) {
      size_t n = strnlen(upkg.content_id, sizeof(upkg.content_id));
      if(n >= content_id_out_size) n = content_id_out_size - 1;
      memcpy(content_id_out, upkg.content_id, n);
      content_id_out[n] = 0;
    }
    return urc;
  }

  /* Same /data → /user/data rewrite for the SDK calls — they hit the
     same install pipeline as DPI does. */
  char sdk_path[1024];
  rewrite_path_for_install(path, sdk_path, sizeof(sdk_path));

  hb_pkg_info_t  pkg_info = {0};
  int rc1 = sceAppInstUtilAppInstallPkg(sdk_path, &pkg_info);
  if(rc1 == 0) {
    if(which_path_out) *which_path_out = 2;
    if(content_id_out && content_id_out_size > 0) {
      size_t n = strnlen(pkg_info.content_id, sizeof(pkg_info.content_id));
      if(n >= content_id_out_size) n = content_id_out_size - 1;
      memcpy(content_id_out, pkg_info.content_id, n);
      content_id_out[n] = 0;
    }
    return 0;
  }

  /* Final fall-back: file:// URI through InstallByPackage — uses the
     rewritten /user/data path for the same sandbox-visibility reason. */
  char file_uri[1024];
  snprintf(file_uri, sizeof(file_uri), "file://%s", sdk_path);

  hb_meta_info_t meta = {0};
  meta.uri          = file_uri;
  meta.ex_uri       = "";
  meta.playgo_scenario_id = "";
  meta.content_id   = "";
  meta.content_name = content_name ? content_name : "Elf Arsenal";
  meta.icon_url     = "";

  hb_pkg_info_t  pkg_info2 = {0};
  hb_playgo_info_t playgo  = {0};

  int rc2 = sceAppInstUtilInstallByPackage(&meta, &pkg_info2, &playgo);
  if(which_path_out) *which_path_out = (rc2 == 0) ? 3 : 0;
  if(rc2 == 0 && content_id_out && content_id_out_size > 0) {
    size_t n = strnlen(pkg_info2.content_id, sizeof(pkg_info2.content_id));
    if(n >= content_id_out_size) n = content_id_out_size - 1;
    memcpy(content_id_out, pkg_info2.content_id, n);
    content_id_out[n] = 0;
  }
  return rc2 != 0 ? rc2 : rc1;
}


#define INSTALL_STATUS_PATH "/data/elf-arsenal/last-install.json"

typedef struct {
  char  path[1024];
  char  name[64];
  int   unlink_after;       /* drop the PKG file once install returns */
} install_job_t;

static void *
install_pkg_thread(void *arg) {
  install_job_t *job = arg;

  jb_escalate_pid(getpid());


  char content_id[64] = {0};
  int which = 0;
  int rc = install_pkg_at_path(job->path, job->name,
                               content_id, sizeof(content_id), &which);

  /* Write final status for the UI to poll. */
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject  (r, "ok",      rc == 0);
  cJSON_AddNumberToObject(r, "result",  rc);
  cJSON_AddStringToObject(r, "via",
        which == 1 ? "dpi" :
        which == 2 ? "sceAppInstUtilAppInstallPkg" :
        which == 3 ? "sceAppInstUtilInstallByPackage" :
        which == 4 ? "sceAppInstUtilInstallByPackage(url)" : "none");
  cJSON_AddStringToObject(r, "path",       job->path);
  cJSON_AddStringToObject(r, "contentId",  content_id);
  cJSON_AddNumberToObject(r, "finishedAt", (double)time(NULL));
  char *txt = cJSON_PrintUnformatted(r);
  if(txt) {
    int fd = open(INSTALL_STATUS_PATH,
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd >= 0) { (void)write(fd, txt, strlen(txt)); close(fd); }
    free(txt);
  }
  cJSON_Delete(r);

  fprintf(stderr, "install_pkg_thread: %s rc=0x%x via=%d cid=%s\n",
          job->path, (unsigned)rc, which,
          content_id[0] ? content_id : "(none)");

  if(job->unlink_after && job->path[0] == '/') {
    if(unlink(job->path) == 0) {
      fprintf(stderr, "install_pkg_thread: cleaned %s\n", job->path);
    }
  }

  free(job);
  return NULL;
}

static void
install_status_mark_started(const char *path) {
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject  (r, "ok",         0);
  cJSON_AddBoolToObject  (r, "pending",    1);
  cJSON_AddStringToObject(r, "path",       path ? path : "");
  cJSON_AddNumberToObject(r, "startedAt",  (double)time(NULL));
  char *txt = cJSON_PrintUnformatted(r);
  if(txt) {
    mkdir("/data/elf-arsenal", 0755);
    int fd = open(INSTALL_STATUS_PATH,
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd >= 0) { (void)write(fd, txt, strlen(txt)); close(fd); }
    free(txt);
  }
  cJSON_Delete(r);
}

static int
install_pkg_async_ex(const char *path, const char *name, int unlink_after) {
  install_job_t *job = calloc(1, sizeof(*job));
  if(!job) return -1;
  strncpy(job->path, path, sizeof(job->path) - 1);
  strncpy(job->name, name ? name : "Elf Arsenal user PKG",
          sizeof(job->name) - 1);
  job->unlink_after = unlink_after;
  install_status_mark_started(path);
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  int rc = pthread_create(&t, &a, install_pkg_thread, job);
  pthread_attr_destroy(&a);
  if(rc != 0) { free(job); return -1; }
  return 0;
}

static int
install_pkg_async(const char *path, const char *name) {
  return install_pkg_async_ex(path, name, 0);
}


static enum MHD_Result
install_launcher_pkg(struct MHD_Connection *conn) {
  ensure_dir("/data/elf-arsenal");
  ensure_dir(PKG_DIR);

  char path[256];
  snprintf(path, sizeof(path),
           PKG_DIR "/elf-arsenal-tile.pkg");

  size_t blen = 0;
  uint8_t *body = http_get(LAUNCHER_PKG_URL, &blen);
  if(!body || blen == 0) {
    free(body);
    return serve_error(conn, MHD_HTTP_BAD_GATEWAY, "download failed");
  }
  if(write_file_bytes(path, body, blen) != 0) {
    free(body);
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "write failed");
  }
  free(body);

  int started = install_pkg_async(path, "Elf Arsenal");

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject  (r, "ok",        started == 0);
  cJSON_AddBoolToObject  (r, "started",   started == 0);
  cJSON_AddStringToObject(r, "path",      path);
  cJSON_AddNumberToObject(r, "size",      (double)blen);
  cJSON_AddStringToObject(r, "statusUrl", "/api/homebrew/install-status");
  cJSON_AddStringToObject(r, "ftpHint",
      "PKG kept on disk. To remove it, FTP into port 2121 (anonymous) "
      "and delete the file at the path above.");
  if(started != 0) cJSON_AddStringToObject(r, "error",
                                          "could not spawn install worker");
  enum MHD_Result ret = serve_json(conn, started == 0 ? MHD_HTTP_OK
                                                      : MHD_HTTP_INTERNAL_SERVER_ERROR, r);
  cJSON_Delete(r);
  return ret;
}


static enum MHD_Result
list_pkgs(struct MHD_Connection *conn) {
  ensure_dir("/data/elf-arsenal");
  ensure_dir(PKG_DIR);
  /* Make pkg dir world-writable so the bundled FTP server can drop files. */
  chmod("/data/elf-arsenal", 0777);
  chmod(PKG_DIR, 0777);

  cJSON *r = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(r, "files");
  DIR *d = opendir(PKG_DIR);
  if(d) {
    struct dirent *ent;
    while((ent = readdir(d))) {
      const char *name = ent->d_name;
      size_t n = strlen(name);
      if(n < 5 || strcasecmp(name + n - 4, ".pkg") != 0) continue;
      char full[512];
      snprintf(full, sizeof(full), "%s/%s", PKG_DIR, name);
      struct stat st;
      cJSON *e = cJSON_CreateObject();
      cJSON_AddStringToObject(e, "name", name);
      cJSON_AddStringToObject(e, "path", full);
      if(stat(full, &st) == 0) {
        cJSON_AddNumberToObject(e, "size", (double)st.st_size);
      }
      cJSON_AddItemToArray(arr, e);
    }
    closedir(d);
  }
  cJSON_AddStringToObject(r, "uploadDir", PKG_DIR);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


/* Install any local .pkg file by absolute path. The path must live
   under /data/ to keep this from being a generic file-walker. */
static enum MHD_Result
install_local_pkg(struct MHD_Connection *conn) {
  const char *path = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                                 "path");
  const char *name = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                                 "name");
  if(!path || !*path) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                       "missing 'path' (FTP your .pkg into "
                       PKG_DIR " first)");
  }
  if(strncmp(path, "/data/", 6) != 0 || strstr(path, "..")) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                       "path must be under /data and contain no '..'");
  }
  struct stat st;
  if(stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
    return serve_error(conn, MHD_HTTP_NOT_FOUND, "no such .pkg file");
  }
  size_t plen = strlen(path);
  if(plen < 5 || strcasecmp(path + plen - 4, ".pkg") != 0) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                       "file does not have a .pkg extension");
  }

  char path_buf[512];
  strncpy(path_buf, path, sizeof(path_buf) - 1);
  path_buf[sizeof(path_buf) - 1] = 0;
  canonicalise_pkg_filename(path_buf, sizeof(path_buf));

  /* Async install — see install_pkg_async() comment block. */
  int started = install_pkg_async(path_buf,
                                  (name && *name) ? name : "Elf Arsenal user PKG");

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject  (r, "ok",        started == 0);
  cJSON_AddBoolToObject  (r, "started",   started == 0);
  cJSON_AddStringToObject(r, "path",      path_buf);
  cJSON_AddNumberToObject(r, "size",      (double)st.st_size);
  cJSON_AddStringToObject(r, "statusUrl", "/api/homebrew/install-status");
  cJSON_AddStringToObject(r, "ftpHint",
      "PKG kept on disk. FTP to port 2121 (anonymous) and delete the "
      "file at the path above to free space.");
  if(started != 0) cJSON_AddStringToObject(r, "error",
                                          "could not spawn install worker");
  enum MHD_Result ret = serve_json(conn, started == 0 ? MHD_HTTP_OK
                                                      : MHD_HTTP_INTERNAL_SERVER_ERROR, r);
  cJSON_Delete(r);
  return ret;
}

static enum MHD_Result
install_pkg_url(struct MHD_Connection *conn) {
  const char *url  = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                                 "url");
  const char *name = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                                 "name");
  if(!url || !*url)
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                       "missing 'url' query argument");
  if(strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                       "url must start with http:// or https://");

  /* Async install — see install_pkg_async() comment block. URL is
     handed to the thread; DPI streams the PKG inside the worker. */
  int started = install_pkg_async(url,
                                  (name && *name) ? name : "Elf Arsenal remote PKG");

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject  (r, "ok",        started == 0);
  cJSON_AddBoolToObject  (r, "started",   started == 0);
  cJSON_AddStringToObject(r, "url",       url);
  cJSON_AddStringToObject(r, "statusUrl", "/api/homebrew/install-status");
  if(started != 0) cJSON_AddStringToObject(r, "error",
                                          "could not spawn install worker");
  enum MHD_Result ret = serve_json(conn, started == 0 ? MHD_HTTP_OK
                                                      : MHD_HTTP_INTERNAL_SERVER_ERROR, r);
  cJSON_Delete(r);
  return ret;
}


typedef struct {
  int    fd;
  char   path[512];
  char   filename[256];
  char   name[160];
  size_t bytes;
  int    install_after;
  int    init_failed;
  char   init_error[160];
} pkg_upload_t;


void
pkg_upload_free(void *state) {
  pkg_upload_t *u = state;
  if(!u) return;
  if(u->fd >= 0) { close(u->fd); u->fd = -1; }
  free(u);
}


/* Reject anything that could escape PKG_UPLOAD_DIR or fool the canonicaliser. */
static int
pkg_filename_is_safe(const char *s) {
  if(!s || !*s) return 0;
  size_t n = strlen(s);
  if(n > 200) return 0;
  if(strstr(s, "..") != NULL) return 0;
  for(size_t i = 0; i < n; i++) {
    char c = s[i];
    if(c == '/' || c == '\\' || c == 0 || c == ':' || c == '<' || c == '>')
      return 0;
  }
  /* Require .pkg extension. */
  if(n < 5 || strcasecmp(s + n - 4, ".pkg") != 0) return 0;
  return 1;
}


static int
rmtree(const char *dir, int keep_root) {
  struct stat st;
  if(lstat(dir, &st) != 0) return -1;
  if(!S_ISDIR(st.st_mode)) return unlink(dir);

  DIR *d = opendir(dir);
  if(!d) return -1;
  struct dirent *ent;
  int rc = 0;
  while((ent = readdir(d)) != NULL) {
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
    char child[1024];
    if((size_t)snprintf(child, sizeof(child), "%s/%s", dir, ent->d_name)
       >= sizeof(child)) continue;
    struct stat cst;
    if(lstat(child, &cst) == 0) {
      if(S_ISDIR(cst.st_mode)) {
        if(rmtree(child, 0) != 0) rc = -1;
      } else {
        if(unlink(child) != 0) rc = -1;
      }
    }
  }
  closedir(d);
  if(!keep_root && rmdir(dir) != 0) rc = -1;
  return rc;
}


void
homebrew_wipe_staged_pkgs(void) {
  rmtree(PKG_DIR, 1 /* keep root */);
  ensure_dir("/data");
  ensure_dir("/data/elf-arsenal");
  ensure_dir(PKG_DIR);
}


enum MHD_Result
pkg_upload_request(struct MHD_Connection *conn,
                   const char *upload_data,
                   size_t *upload_data_size,
                   void **state) {
  pkg_upload_t *u = *state;

  if(!u) {
    u = calloc(1, sizeof(*u));
    if(!u) return MHD_NO;
    u->fd = -1;
    u->install_after = 1;
    *state = u;

    const char *filename = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "filename");
    const char *name     = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "name");
    const char *install  = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "install");
    if(install && (!strcmp(install, "0") || !strcasecmp(install, "false")))
      u->install_after = 0;

    if(!pkg_filename_is_safe(filename)) {
      u->init_failed = 1;
      strncpy(u->init_error,
              "missing or unsafe ?filename= (must be safe basename ending in .pkg)",
              sizeof(u->init_error) - 1);
      return MHD_YES;
    }
    strncpy(u->filename, filename, sizeof(u->filename) - 1);
    if(name && *name) strncpy(u->name, name, sizeof(u->name) - 1);

    ensure_dir("/data");
    ensure_dir("/data/elf-arsenal");
    ensure_dir(PKG_DIR);
    ensure_dir(PKG_UPLOAD_DIR);

    if(snprintf(u->path, sizeof(u->path), "%s/%s",
                PKG_UPLOAD_DIR, u->filename) >= (int)sizeof(u->path)) {
      u->init_failed = 1;
      strncpy(u->init_error, "filename too long",
              sizeof(u->init_error) - 1);
      u->path[0] = 0;
      return MHD_YES;
    }
    u->fd = open(u->path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(u->fd < 0) {
      u->init_failed = 1;
      snprintf(u->init_error, sizeof(u->init_error),
               "open %s: %s", u->path, strerror(errno));
      u->path[0] = 0;
      return MHD_YES;
    }
    return MHD_YES;
  }

  /* Streaming body chunks. */
  if(*upload_data_size > 0) {
    if(!u->init_failed && u->fd >= 0) {
      size_t want = *upload_data_size, off = 0;
      while(off < want) {
        ssize_t w = write(u->fd, upload_data + off, want - off);
        if(w <= 0) {
          u->init_failed = 1;
          snprintf(u->init_error, sizeof(u->init_error),
                   "write: %s", strerror(errno));
          break;
        }
        off += (size_t)w;
      }
      u->bytes += off;
    }
    *upload_data_size = 0;
    return MHD_YES;
  }

  /* End-of-body — close, validate, optionally install. */
  if(u->fd >= 0) { close(u->fd); u->fd = -1; }

  cJSON *r = cJSON_CreateObject();
  if(u->init_failed || u->bytes == 0) {
    cJSON_AddBoolToObject(r, "ok", 0);
    cJSON_AddStringToObject(r, "error",
        u->init_failed ? u->init_error : "empty upload");
    enum MHD_Result ret = serve_json(conn, MHD_HTTP_BAD_REQUEST, r);
    cJSON_Delete(r);
    pkg_upload_free(u);
    *state = NULL;
    return ret;
  }

  /* PKG header sanity check + canonicalise to <ContentID>.pkg. */
  char path_buf[512];
  strncpy(path_buf, u->path, sizeof(path_buf) - 1);
  path_buf[sizeof(path_buf) - 1] = 0;
  canonicalise_pkg_filename(path_buf, sizeof(path_buf));

  cJSON_AddStringToObject(r, "uploadedAs", path_buf);
  cJSON_AddNumberToObject(r, "size",       (double)u->bytes);

  int started = 0;
  if(u->install_after) {
    started = install_pkg_async_ex(path_buf,
        u->name[0] ? u->name : "Elf Arsenal uploaded PKG", 1);
    cJSON_AddBoolToObject  (r, "started",   started == 0);
    cJSON_AddStringToObject(r, "statusUrl", "/api/homebrew/install-status");
  } else {
    cJSON_AddBoolToObject(r, "installSkipped", 1);
    /* No install means we own the cleanup decision right here. */
    if(unlink(path_buf) == 0) cJSON_AddBoolToObject(r, "cleaned", 1);
    else                      cJSON_AddBoolToObject(r, "cleaned", 0);
  }

  int ok = (u->install_after ? started == 0 : 1);
  cJSON_AddBoolToObject(r, "ok", ok);
  if(!ok) {
    cJSON_AddStringToObject(r, "error",
        "Upload landed but could not spawn the install worker.");
  }
  enum MHD_Result ret = serve_json(conn, ok ? MHD_HTTP_OK
                                            : MHD_HTTP_INTERNAL_SERVER_ERROR, r);
  cJSON_Delete(r);
  pkg_upload_free(u);
  *state = NULL;
  return ret;
}


static enum MHD_Result
install_status_request(struct MHD_Connection *conn) {
  int fd = open(INSTALL_STATUS_PATH, O_RDONLY);
  if(fd < 0) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok",   1);
    cJSON_AddBoolToObject(r, "idle", 1);
    enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
    cJSON_Delete(r);
    return ret;
  }
  char buf[2048];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if(n <= 0) {
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                       "could not read install status");
  }
  buf[n] = 0;
  struct MHD_Response *resp =
      MHD_create_response_from_buffer(n, buf, MHD_RESPMEM_MUST_COPY);
  if(!resp) return MHD_NO;
  MHD_add_response_header(resp, "Content-Type",  "application/json");
  MHD_add_response_header(resp, "Cache-Control", "no-cache");
  enum MHD_Result ret = websrv_queue_response(conn, MHD_HTTP_OK, resp);
  MHD_destroy_response(resp);
  return ret;
}


/* GET /api/homebrew/dpi-status — diagnostic for the UI. */
static enum MHD_Result
dpi_status_request(struct MHD_Connection *conn) {
  int v1 = sys_port_is_open(9040);
  int v2 = sys_dpiv2_is_running();
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r,   "running",   v1 || v2);
  cJSON_AddBoolToObject(r,   "v1Running", v1);
  cJSON_AddBoolToObject(r,   "v2Running", v2);
  cJSON_AddStringToObject(r, "endpoint",  v2 ? "0.0.0.0:12800" : "127.0.0.1:9040");
  cJSON_AddStringToObject(r, "version",   v2 ? "v2" : (v1 ? "v1" : "none"));
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


/* GET /api/homebrew/dpi-start — one-shot respawn. */
static enum MHD_Result
dpi_start_request(struct MHD_Connection *conn) {
  int rc = sys_dpi_ensure_running();
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", rc >= 0);
  cJSON_AddBoolToObject(r, "running", sys_port_is_open(9040));
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


/* GET /api/homebrew/dpi-probe — send a dummy URL to DPI and return the raw
   response so we can see the exact error code on higher firmware. */
static enum MHD_Result
dpi_probe_request(struct MHD_Connection *conn) {
  sys_dpi_ensure_running();
  char raw[256] = {0};
  int rc = dpi_send_install("http://0.0.0.0/probe.pkg", raw, sizeof(raw));
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r,   "dpiRunning",  sys_port_is_open(9040));
  cJSON_AddBoolToObject(r,   "connectOk",   rc == 0);
  cJSON_AddStringToObject(r, "rawResponse", rc == 0 ? raw : "(connect failed)");
  /* Parse error code if DPI returned "error:0x..." */
  if(rc == 0 && strncmp(raw, "error:", 6) == 0) {
    cJSON_AddStringToObject(r, "errorCode", raw + 6);
  } else if(rc == 0 && strncmp(raw, "ok", 2) == 0) {
    cJSON_AddStringToObject(r, "errorCode", "none — install accepted");
  }
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


/*  Dispatcher                                                            */

enum MHD_Result
homebrew_request(struct MHD_Connection *conn, const char *url) {
  if(!strcmp(url, "/api/homebrew/list")) {
    return list_assets(conn);
  }
  if(!strcmp(url, "/api/homebrew/install")) {
    return install_asset(conn);
  }
  if(!strcmp(url, "/api/homebrew/launcher")) {
    return install_launcher_pkg(conn);
  }
  if(!strcmp(url, "/api/homebrew/pkgs")) {
    return list_pkgs(conn);
  }
  if(!strcmp(url, "/api/homebrew/install-pkg")) {
    return install_local_pkg(conn);
  }
  if(!strcmp(url, "/api/homebrew/install-pkg-url")) {
    return install_pkg_url(conn);
  }
  if(!strcmp(url, "/api/homebrew/install-status")) {
    return install_status_request(conn);
  }
  if(!strcmp(url, "/api/homebrew/dpi-status")) {
    return dpi_status_request(conn);
  }
  if(!strcmp(url, "/api/homebrew/dpi-start")) {
    return dpi_start_request(conn);
  }
  if(!strcmp(url, "/api/homebrew/dpi-probe")) {
    return dpi_probe_request(conn);
  }
  return serve_error(conn, MHD_HTTP_NOT_FOUND, "no such endpoint");
}


/*  Boot-time auto-install of the Elf Arsenal home-screen tile     */

#include <sys/syscall.h>

static atomic_int g_tile_autoinstall = 0;

int
homebrew_tile_autoinstall_enabled(void) {
  return atomic_load(&g_tile_autoinstall);
}

void
homebrew_tile_autoinstall_set_enabled(int on) {
  int prev = atomic_exchange(&g_tile_autoinstall, on ? 1 : 0);
  extern void config_save(void);
  config_save();
  if (!prev && on) {
    homebrew_auto_install_tile_init();
  }
}

static void *
auto_install_tile_thread(void *arg) {
  (void)arg;
  syscall(SYS_thr_set_name, -1, "tile-autoinst");

  if (!atomic_load(&g_tile_autoinstall)) {
    fprintf(stderr, "tile-autoinst: skipped (disabled by user)\n");
    return NULL;
  }

  /* Skip if PSPS69691 tile is already installed — avoids triggering
     AppPrepareOverwriteByPackage every boot, which crashes SceShellCore's
     SceAppInstallerJobQueue via an uncaught std::out_of_range. */
  struct stat tile_st;
  if (stat("/user/appmeta/PSPS69691", &tile_st) == 0) {
    fprintf(stderr, "tile-autoinst: PSPS69691 already installed, skipping\n");
    return NULL;
  }

  sleep(30);

  /* Re-check after the sleep — user may have flipped the toggle off
     while we were waiting. */
  if (!atomic_load(&g_tile_autoinstall)) {
    fprintf(stderr, "tile-autoinst: skipped (disabled mid-wait)\n");
    return NULL;
  }

  /* Install via the native download manager (sceAppInstUtilInstallByPackage
     with an HTTPS URL) so the progress bar shows on the home screen.
     install_pkg_at_path already has the URL branch; we just hand it the
     remote URL directly instead of downloading locally first. */
  fprintf(stderr, "tile-autoinst: installing from %s\n", LAUNCHER_PKG_URL);
  char content_id[64] = {0};
  int which = 0;
  int rc = install_pkg_at_path(LAUNCHER_PKG_URL, "Elf Arsenal",
                               content_id, sizeof(content_id), &which);
  if(rc == 0) {
    fprintf(stderr, "tile-autoinst: installed via %s%s%s\n",
            which == 1 ? "DPI" :
            which == 2 ? "sceAppInstUtilAppInstallPkg" :
            which == 3 ? "sceAppInstUtilInstallByPackage" :
            which == 4 ? "sceAppInstUtilInstallByPackage(url)" : "?",
            content_id[0] ? " — " : "",
            content_id[0] ? content_id : "");
    notify("Elf Arsenal: home-screen tile installed");
  } else {
    fprintf(stderr, "tile-autoinst: install rc=0x%08x\n", (unsigned)rc);
    notify("Elf Arsenal: tile install failed (0x%08x)", (unsigned)rc);
  }
  return NULL;
}


void
homebrew_auto_install_tile_init(void) {
  pthread_t t;
  if(pthread_create(&t, NULL, auto_install_tile_thread, NULL) != 0) {
    perror("homebrew_auto_install_tile_init: pthread_create");
    return;
  }
  pthread_detach(t);
}
