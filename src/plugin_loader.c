/* Arsenal Plugin Loader
   Scans /data/elf-arsenal/plugins/ for *.elf files at boot and spawns each
   enabled one with full jb credentials. Inspired by etaHEN/GoldHEN's plugin
   system; works on any firmware Arsenal supports (etaHEN tops out at 10.01).

   Directory layout:
     /data/elf-arsenal/plugins/       <- drop *.elf here
     /data/elf-arsenal/plugins/filez/ <- file-level patches/replacements
     /data/elf-arsenal/plugins/krnlz/ <- kernel-level patch data
*/

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <microhttpd.h>

#include "config.h"
#include "fs.h"
#include "plugin_loader.h"
#include "ps5/elfldr.h"
#include "ps5/notify.h"
#include "jb.h"
#include "websrv.h"

#define PLUGIN_DIR      "/data/elf-arsenal/plugins"
#define FILEZ_DIR       PLUGIN_DIR "/filez"
#define KRNLZ_DIR       PLUGIN_DIR "/krnlz"
#define MAX_PLUGINS     32
#define MAX_NAME        64

typedef struct {
  char   name[MAX_NAME];   /* basename without .elf */
  char   path[256];        /* full path */
  int    enabled;          /* persisted on/off */
  pid_t  pid;              /* 0 = not running */
} plugin_t;

static pthread_mutex_t g_lock   = PTHREAD_MUTEX_INITIALIZER;
static plugin_t        g_plugins[MAX_PLUGINS];
static int             g_count  = 0;


/* ── helpers ── */

static int
ends_with_elf(const char *s) {
  size_t n = strlen(s);
  return n > 4 && !strcmp(s + n - 4, ".elf");
}

static plugin_t *
find_plugin(const char *name) {
  for(int i = 0; i < g_count; i++)
    if(!strcmp(g_plugins[i].name, name))
      return &g_plugins[i];
  return NULL;
}

/* Spawn the ELF at path, escalate, return pid or -1. */
static pid_t
spawn_plugin_elf(const char *path, const char *name) {
  size_t sz = 0;
  uint8_t *elf = fs_readfile(path, &sz);
  if(!elf || sz < 4) {
    fprintf(stderr, "plugin_loader: cannot read %s\n", path);
    free(elf);
    return -1;
  }

  char *argv[2] = {(char *)name, NULL};
  char *envp[1] = {NULL};

  int devnull = open("/dev/null", O_WRONLY);
  pid_t pid = elfldr_spawn("/", devnull >= 0 ? devnull : -1, elf, argv, envp);
  if(devnull >= 0) close(devnull);
  free(elf);

  if(pid < 0) {
    fprintf(stderr, "plugin_loader: elfldr_spawn failed for %s\n", name);
    return -1;
  }

  jb_escalate_pid(pid);
  fprintf(stderr, "plugin_loader: spawned %s (pid=%d)\n", name, (int)pid);
  return pid;
}

/* Rescan /data/elf-arsenal/plugins/ and add any new .elf files to g_plugins.
   Must be called with g_lock held. */
static void
rescan_locked(void) {
  DIR *d = opendir(PLUGIN_DIR);
  if(!d) return;

  struct dirent *de;
  while((de = readdir(d))) {
    if(!ends_with_elf(de->d_name)) continue;

    /* Derive name (strip .elf) */
    char name[MAX_NAME];
    size_t nl = strlen(de->d_name) - 4;
    if(nl >= MAX_NAME) nl = MAX_NAME - 1;
    memcpy(name, de->d_name, nl);
    name[nl] = 0;

    /* Already known? */
    if(find_plugin(name)) continue;

    if(g_count >= MAX_PLUGINS) {
      fprintf(stderr, "plugin_loader: too many plugins (max %d)\n", MAX_PLUGINS);
      break;
    }

    plugin_t *p = &g_plugins[g_count++];
    memset(p, 0, sizeof(*p));
    strncpy(p->name, name, MAX_NAME - 1);
    snprintf(p->path, sizeof(p->path), PLUGIN_DIR "/%s", de->d_name);
    p->enabled = 1;  /* new plugins default to enabled */
    p->pid     = 0;
  }
  closedir(d);
}


/* ── public API ── */

void
plugin_loader_init(void) {
  /* Create directory tree */
  mkdir("/data", 0755);
  mkdir(PLUGIN_DIR, 0755);
  mkdir(FILEZ_DIR,  0755);
  mkdir(KRNLZ_DIR,  0755);

  pthread_mutex_lock(&g_lock);
  rescan_locked();

  int launched = 0;
  for(int i = 0; i < g_count; i++) {
    plugin_t *p = &g_plugins[i];
    if(!p->enabled) {
      fprintf(stderr, "plugin_loader: %s disabled, skipping\n", p->name);
      continue;
    }
    pid_t pid = spawn_plugin_elf(p->path, p->name);
    if(pid > 0) { p->pid = pid; launched++; }
  }
  pthread_mutex_unlock(&g_lock);

  if(launched > 0)
    notify("Elf Arsenal: loaded %d plugin%s", launched, launched == 1 ? "" : "s");
}

char *
plugin_loader_list_json(void) {
  pthread_mutex_lock(&g_lock);
  rescan_locked();

  /* Check which pids are still alive */
  for(int i = 0; i < g_count; i++) {
    plugin_t *p = &g_plugins[i];
    if(p->pid > 0 && kill(p->pid, 0) != 0)
      p->pid = 0;
  }

  /* Build JSON */
  size_t cap = 256 + g_count * 192;
  char *out = malloc(cap);
  if(!out) { pthread_mutex_unlock(&g_lock); return NULL; }

  size_t pos = 0;
  out[pos++] = '[';
  for(int i = 0; i < g_count; i++) {
    plugin_t *p = &g_plugins[i];
    if(i) out[pos++] = ',';
    int n = snprintf(out + pos, cap - pos,
      "{\"name\":\"%s\",\"path\":\"%s\",\"enabled\":%s,\"running\":%s,\"pid\":%d}",
      p->name, p->path,
      p->enabled ? "true" : "false",
      p->pid > 0  ? "true" : "false",
      (int)p->pid);
    if(n > 0) pos += (size_t)n;
  }
  out[pos++] = ']';
  out[pos]   = 0;

  pthread_mutex_unlock(&g_lock);
  return out;
}

int
plugin_loader_spawn(const char *name) {
  pthread_mutex_lock(&g_lock);
  rescan_locked();
  plugin_t *p = find_plugin(name);
  if(!p) { pthread_mutex_unlock(&g_lock); return -1; }

  /* Kill stale pid if process is dead */
  if(p->pid > 0 && kill(p->pid, 0) != 0) p->pid = 0;
  if(p->pid > 0) { pthread_mutex_unlock(&g_lock); return 0; }  /* already running */

  pid_t pid = spawn_plugin_elf(p->path, p->name);
  if(pid > 0) p->pid = pid;
  pthread_mutex_unlock(&g_lock);
  return pid > 0 ? 0 : -1;
}

int
plugin_loader_kill(const char *name) {
  pthread_mutex_lock(&g_lock);
  plugin_t *p = find_plugin(name);
  if(!p || p->pid <= 0) { pthread_mutex_unlock(&g_lock); return -1; }

  pid_t target = p->pid;
  int dead = 0;
  for(int i = 0; i < 10 && !dead; i++) {
    if(kill(target, SIGKILL) != 0 && errno == ESRCH) { dead = 1; break; }
    struct timespec ts = {0, 50 * 1000 * 1000L};
    nanosleep(&ts, NULL);
    if(kill(target, 0) != 0 && errno == ESRCH) dead = 1;
  }
  if(dead) {
    fprintf(stderr, "plugin_loader: killed %s (pid=%d)\n", name, (int)target);
    p->pid = 0;
  }
  pthread_mutex_unlock(&g_lock);
  return dead ? 0 : -1;
}

int
plugin_loader_set_enabled(const char *name, int on) {
  pthread_mutex_lock(&g_lock);
  rescan_locked();
  plugin_t *p = find_plugin(name);
  if(!p) { pthread_mutex_unlock(&g_lock); return -1; }
  p->enabled = on ? 1 : 0;
  pthread_mutex_unlock(&g_lock);
  return 0;
}

int
plugin_loader_get_enabled(const char *name) {
  pthread_mutex_lock(&g_lock);
  plugin_t *p = find_plugin(name);
  int v = p ? p->enabled : 1;
  pthread_mutex_unlock(&g_lock);
  return v;
}

void
plugin_loader_config_serialize(FILE *f) {
  pthread_mutex_lock(&g_lock);
  for(int i = 0; i < g_count; i++)
    fprintf(f, "plugin_%s=%d\n", g_plugins[i].name, g_plugins[i].enabled ? 1 : 0);
  pthread_mutex_unlock(&g_lock);
}

void
plugin_loader_config_apply(const char *name, int enabled) {
  pthread_mutex_lock(&g_lock);
  plugin_t *p = find_plugin(name);
  if(p) p->enabled = enabled ? 1 : 0;
  pthread_mutex_unlock(&g_lock);
}


/* ── HTTP request handler: /api/plugins/... ── */

static enum MHD_Result
pl_json_resp(struct MHD_Connection *conn, unsigned int status,
             const char *body) {
  struct MHD_Response *r = MHD_create_response_from_buffer(
    strlen(body), (void*)body, MHD_RESPMEM_PERSISTENT);
  MHD_add_response_header(r, "Content-Type", "application/json");
  MHD_add_response_header(r, "Cache-Control", "no-cache");
  enum MHD_Result rc = websrv_queue_response(conn, status, r);
  MHD_destroy_response(r);
  return rc;
}

enum MHD_Result
plugin_loader_request(struct MHD_Connection *conn, const char *url) {
  /* GET /api/plugins/list */
  if(!strcmp(url, "/api/plugins/list")) {
    char *json = plugin_loader_list_json();
    if(!json) return pl_json_resp(conn, 500, "{\"error\":\"alloc\"}");
    struct MHD_Response *r = MHD_create_response_from_buffer(
      strlen(json), json, MHD_RESPMEM_MUST_FREE);
    MHD_add_response_header(r, "Content-Type", "application/json");
    MHD_add_response_header(r, "Cache-Control", "no-cache");
    enum MHD_Result rc = websrv_queue_response(conn, MHD_HTTP_OK, r);
    MHD_destroy_response(r);
    return rc;
  }

  /* GET /api/plugins/spawn?name=<name> */
  if(!strcmp(url, "/api/plugins/spawn")) {
    const char *name = MHD_lookup_connection_value(
      conn, MHD_GET_ARGUMENT_KIND, "name");
    if(!name) return pl_json_resp(conn, 400, "{\"ok\":false,\"error\":\"missing name\"}");
    int rc = plugin_loader_spawn(name);
    return pl_json_resp(conn, 200,
      rc == 0 ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"spawn failed\"}");
  }

  /* GET /api/plugins/kill?name=<name> */
  if(!strcmp(url, "/api/plugins/kill")) {
    const char *name = MHD_lookup_connection_value(
      conn, MHD_GET_ARGUMENT_KIND, "name");
    if(!name) return pl_json_resp(conn, 400, "{\"ok\":false,\"error\":\"missing name\"}");
    int rc = plugin_loader_kill(name);
    return pl_json_resp(conn, 200,
      rc == 0 ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"kill failed\"}");
  }

  /* GET /api/plugins/enable?name=<name>&on=1|0 */
  if(!strcmp(url, "/api/plugins/enable")) {
    const char *name = MHD_lookup_connection_value(
      conn, MHD_GET_ARGUMENT_KIND, "name");
    const char *on_s = MHD_lookup_connection_value(
      conn, MHD_GET_ARGUMENT_KIND, "on");
    if(!name) return pl_json_resp(conn, 400, "{\"ok\":false,\"error\":\"missing name\"}");
    int on = on_s ? atoi(on_s) : 1;
    plugin_loader_set_enabled(name, on);
    config_save();
    return pl_json_resp(conn, 200, "{\"ok\":true}");
  }

  return pl_json_resp(conn, 404, "{\"error\":\"not found\"}");
}
