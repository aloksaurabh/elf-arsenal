#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <microhttpd.h>

#include "fs.h"
#include "websrv.h"
#include "xml_patches.h"

/* ── helpers ── */

static enum MHD_Result
json_resp(struct MHD_Connection *conn, unsigned int status, const char *body) {
  struct MHD_Response *r = MHD_create_response_from_buffer(
    strlen(body), (void *)body, MHD_RESPMEM_MUST_COPY);
  MHD_add_response_header(r, "Content-Type", "application/json");
  MHD_add_response_header(r, "Cache-Control", "no-cache");
  enum MHD_Result rc = websrv_queue_response(conn, status, r);
  MHD_destroy_response(r);
  return rc;
}

/* All three directories CheatRunner scans for XML patches. */
static const char * const k_xml_dirs[][2] = {
  { "/data/elf-arsenal/patches/xml",          "/data/elf-arsenal/patches/xml.off"          },
  { "/data/cheatrunner/patches/xml",          "/data/cheatrunner/patches/xml.off"          },
  { "/data/cheatrunner/patches/xml_prospero", "/data/cheatrunner/patches/xml_prospero.off" },
};
#define N_XML_DIRS ((int)(sizeof(k_xml_dirs)/sizeof(k_xml_dirs[0])))

static const char * const k_xml_sources[] = {
  "Arsenal", "CheatRunner", "CheatRunner PS5"
};

/* Return the active path for a given dir slot (live if exists, else .off). */
static const char *
active_dir_for(int idx) {
  struct stat st;
  if(stat(k_xml_dirs[idx][0], &st) == 0 && S_ISDIR(st.st_mode))
    return k_xml_dirs[idx][0];
  return k_xml_dirs[idx][1];
}

int
xml_patches_global_enabled(void) {
  struct stat st;
  return stat(XML_PATCHES_DIR, &st) == 0 && S_ISDIR(st.st_mode);
}

void
xml_patches_set_global(int on) {
  struct stat st;
  for(int i = 0; i < N_XML_DIRS; i++) {
    const char *live = k_xml_dirs[i][0];
    const char *off  = k_xml_dirs[i][1];
    if(on) {
      if(stat(off, &st) == 0 && S_ISDIR(st.st_mode))
        rename(off, live);
      else if(stat(live, &st) != 0)
        mkdir(live, 0755);
      chmod(live, 0777);
    } else {
      if(stat(live, &st) == 0 && S_ISDIR(st.st_mode))
        rename(live, off);
    }
  }
}

/* Returns a heap-allocated JSON array of patch file objects across all dirs. */
char *
xml_patches_list_json(void) {
  /* Count total entries for buffer sizing. */
  size_t count = 0;
  for(int i = 0; i < N_XML_DIRS; i++) {
    const char *dir = active_dir_for(i);
    DIR *d = opendir(dir);
    if(!d) continue;
    struct dirent *de;
    while((de = readdir(d))) {
      size_t nl = strlen(de->d_name);
      if((nl > 4 && !strcmp(de->d_name + nl - 4, ".xml")) ||
         (nl > 8 && !strcmp(de->d_name + nl - 8, ".xml.off")))
        count++;
    }
    closedir(d);
  }

  int enabled_global = xml_patches_global_enabled();
  size_t cap = 64 + count * 256;
  char *out = malloc(cap);
  if(!out) return NULL;

  size_t pos = 0;
  out[pos++] = '[';
  int first = 1;

  for(int i = 0; i < N_XML_DIRS; i++) {
    const char *dir = active_dir_for(i);
    DIR *d = opendir(dir);
    if(!d) continue;
    struct dirent *de;
    while((de = readdir(d))) {
      const char *n = de->d_name;
      size_t nl = strlen(n);
      int is_xml = nl > 4 && !strcmp(n + nl - 4, ".xml");
      int is_off = nl > 8 && !strcmp(n + nl - 8, ".xml.off");
      if(!is_xml && !is_off) continue;

      char display[256];
      size_t dl = is_off ? nl - 8 : nl - 4;
      if(dl >= sizeof(display)) dl = sizeof(display) - 1;
      memcpy(display, n, dl);
      display[dl] = 0;

      /* Escape display name for JSON. */
      char escaped[512];
      size_t ei = 0;
      for(size_t j = 0; j < dl && ei < sizeof(escaped) - 2; j++) {
        char c = display[j];
        if(c == '"' || c == '\\') escaped[ei++] = '\\';
        escaped[ei++] = c;
      }
      escaped[ei] = 0;

      if(!first) out[pos++] = ',';
      first = 0;

      int n2 = snprintf(out + pos, cap - pos,
        "{\"name\":\"%s\",\"file\":\"%s\",\"enabled\":%s,"
        "\"globalEnabled\":%s,\"dir\":%d,\"source\":\"%s\"}",
        escaped, n,
        is_xml ? "true" : "false",
        enabled_global ? "true" : "false",
        i, k_xml_sources[i]);
      if(n2 > 0) pos += (size_t)n2;
    }
    closedir(d);
  }

  out[pos++] = ']';
  out[pos]   = 0;
  return out;
}

/* Validate a filename: only alnum, '.', '_', '-' allowed; no path separators. */
static int
valid_xml_name(const char *s) {
  if(!s || !*s) return 0;
  for(const char *p = s; *p; p++) {
    char c = *p;
    if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
       (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
      continue;
    return 0;
  }
  /* Must end in .xml or .xml.off */
  size_t n = strlen(s);
  int ok = (n > 4 && !strcmp(s + n - 4, ".xml")) ||
           (n > 8 && !strcmp(s + n - 8, ".xml.off"));
  return ok;
}

int
xml_patches_toggle(const char *name, int on, int dir_idx) {
  if(!valid_xml_name(name)) return -1;
  if(dir_idx < 0 || dir_idx >= N_XML_DIRS) return -1;
  const char *dir = active_dir_for(dir_idx);

  char from[512], to[512];
  if(on) {
    size_t nl = strlen(name);
    if(nl < 8 || strcmp(name + nl - 8, ".xml.off") != 0) return -1;
    char base[256];
    size_t bl = nl - 4;
    if(bl >= sizeof(base)) return -1;
    memcpy(base, name, bl); base[bl] = 0;
    snprintf(from, sizeof(from), "%s/%s", dir, name);
    snprintf(to,   sizeof(to),   "%s/%s", dir, base);
  } else {
    size_t nl = strlen(name);
    if(nl < 4 || strcmp(name + nl - 4, ".xml") != 0) return -1;
    snprintf(from, sizeof(from), "%s/%s",     dir, name);
    snprintf(to,   sizeof(to),   "%s/%s.off", dir, name);
  }
  return rename(from, to) == 0 ? 0 : -1;
}

int
xml_patches_delete(const char *name, int dir_idx) {
  if(!valid_xml_name(name)) return -1;
  if(dir_idx < 0 || dir_idx >= N_XML_DIRS) return -1;
  const char *dir = active_dir_for(dir_idx);
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  return unlink(path) == 0 ? 0 : -1;
}


/* ── Upload handler ── */

typedef struct {
  int    fd;
  char   path[512];
  char   error[128];
  int    failed;
  size_t bytes;
} xml_upload_t;

enum MHD_Result
xml_patches_upload_request(struct MHD_Connection *conn,
                           const char *upload_data,
                           size_t *upload_data_size,
                           void **state) {
  xml_upload_t *u = *state;

  /* Phase 1: initialise. */
  if(!u) {
    u = calloc(1, sizeof(*u));
    if(!u) return MHD_NO;
    u->fd = -1;
    *state = u;

    const char *fname = MHD_lookup_connection_value(
      conn, MHD_GET_ARGUMENT_KIND, "name");
    if(!fname || !*fname) {
      snprintf(u->error, sizeof(u->error), "missing ?name= parameter");
      u->failed = 1;
      return MHD_YES;
    }

    /* Sanitise: must be <something>.xml */
    size_t fnl = strlen(fname);
    if(fnl < 5 || strcmp(fname + fnl - 4, ".xml") != 0 ||
       !valid_xml_name(fname)) {
      snprintf(u->error, sizeof(u->error),
               "name must be a valid *.xml filename");
      u->failed = 1;
      return MHD_YES;
    }

    const char *dir = active_dir_for(0);
    snprintf(u->path, sizeof(u->path), "%s/%s", dir, fname);
    u->fd = open(u->path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(u->fd < 0) {
      snprintf(u->error, sizeof(u->error), "open: %s", strerror(errno));
      u->failed = 1;
      u->path[0] = 0;
    }
    return MHD_YES;
  }

  /* Phase 2: stream body. */
  if(*upload_data_size > 0) {
    if(!u->failed && u->fd >= 0) {
      size_t want = *upload_data_size, off = 0;
      while(off < want) {
        ssize_t w = write(u->fd, upload_data + off, want - off);
        if(w <= 0) {
          snprintf(u->error, sizeof(u->error), "write: %s", strerror(errno));
          u->failed = 1;
          break;
        }
        off += (size_t)w;
      }
      u->bytes += off;
    }
    *upload_data_size = 0;
    return MHD_YES;
  }

  /* Phase 3: done — send response and clean up. */
  if(u->fd >= 0) close(u->fd);
  u->fd = -1;

  static const char ok_body[]  = "{\"ok\":true}";
  static const char err_pre[]  = "{\"ok\":false,\"error\":\"";
  char body[256];

  enum MHD_Result rc;
  if(u->failed) {
    if(u->path[0]) unlink(u->path);
    snprintf(body, sizeof(body), "%s%s\"}", err_pre, u->error);
    rc = json_resp(conn, 400, body);
  } else {
    rc = json_resp(conn, 200, ok_body);
  }

  free(u);
  *state = NULL;
  return rc;
}


/* ── HTTP request handler: GET /api/xmlpatches/... ── */

enum MHD_Result
xml_patches_request(struct MHD_Connection *conn, const char *url) {
  /* GET /api/xmlpatches/list */
  if(!strcmp(url, "/api/xmlpatches/list")) {
    char *json = xml_patches_list_json();
    if(!json) return json_resp(conn, 500, "{\"error\":\"alloc\"}");
    struct MHD_Response *r = MHD_create_response_from_buffer(
      strlen(json), json, MHD_RESPMEM_MUST_FREE);
    MHD_add_response_header(r, "Content-Type", "application/json");
    MHD_add_response_header(r, "Cache-Control", "no-cache");
    enum MHD_Result rc = websrv_queue_response(conn, MHD_HTTP_OK, r);
    MHD_destroy_response(r);
    return rc;
  }

  /* GET /api/xmlpatches/global?on=1|0 */
  if(!strcmp(url, "/api/xmlpatches/global")) {
    const char *on_s = MHD_lookup_connection_value(
      conn, MHD_GET_ARGUMENT_KIND, "on");
    if(!on_s) {
      /* Query only — return current state. */
      char body[64];
      snprintf(body, sizeof(body), "{\"enabled\":%s}",
               xml_patches_global_enabled() ? "true" : "false");
      return json_resp(conn, 200, body);
    }
    xml_patches_set_global(atoi(on_s));
    return json_resp(conn, 200, "{\"ok\":true}");
  }

  /* GET /api/xmlpatches/toggle?name=<file>&on=1|0&dir=0|1|2 */
  if(!strcmp(url, "/api/xmlpatches/toggle")) {
    const char *name  = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "name");
    const char *on_s  = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "on");
    const char *dir_s = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "dir");
    if(!name || !on_s)
      return json_resp(conn, 400, "{\"ok\":false,\"error\":\"missing name or on\"}");
    int dir_idx = dir_s ? atoi(dir_s) : 0;
    int rc = xml_patches_toggle(name, atoi(on_s), dir_idx);
    return json_resp(conn, 200,
      rc == 0 ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"rename failed\"}");
  }

  /* GET /api/xmlpatches/delete?name=<file>&dir=0|1|2 */
  if(!strcmp(url, "/api/xmlpatches/delete")) {
    const char *name  = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "name");
    const char *dir_s = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "dir");
    if(!name)
      return json_resp(conn, 400, "{\"ok\":false,\"error\":\"missing name\"}");
    int dir_idx = dir_s ? atoi(dir_s) : 0;
    int rc = xml_patches_delete(name, dir_idx);
    return json_resp(conn, 200,
      rc == 0 ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"delete failed\"}");
  }

  return json_resp(conn, 404, "{\"error\":\"not found\"}");
}
