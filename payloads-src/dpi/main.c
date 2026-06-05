/* DPI payload — Direct Package Installer
 * Listens on TCP 9040, accepts plain URL or file:// path, calls
 * sceAppInstUtilInstallByPackage.  Compiled with the current PS5 payload
 * SDK so kernel-offset tables cover all supported FW versions.
 *
 * Boot-timing fix: kstuff spawns last in the payload chain.  DPI sleeps up
 * to 25 s at startup to let kstuff apply its kernel patches before we try
 * sceAppInstUtilInitialize().  On FW < 11.00 init succeeds after the
 * patches land; on FW 11.00+ it still fails (SYSTEM_AUTHID required in the
 * spawned process) and Arsenal's privileged in-process fallback takes over.
 *
 * Retry: if the startup init timed out (IPMI service not ready yet), DPI
 * retries once on each incoming install request so a late-starting IPMI
 * backend is picked up automatically.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define DPI_PORT      9040
#define INIT_TIMEOUT  10   /* seconds to wait for sceAppInstUtilInitialize */

/* ── sceAppInstUtil ABI ──────────────────────────────────────────────── */
#define NUM_LANGUAGES   30
#define NUM_IDS         64
#define SCENARIOID_SIZE 3
#define CONTENTID_SIZE  0x30
#define LANGUAGE_SIZE   8

typedef char scenario_id_t[SCENARIOID_SIZE];
typedef char language_t[LANGUAGE_SIZE];
typedef char content_id_t[CONTENTID_SIZE];

typedef struct {
  content_id_t content_id;
  int          content_type;
  int          content_platform;
} pkg_info_t;

typedef struct {
  const char *uri;
  const char *ex_uri;
  const char *playgo_scenario_id;
  const char *content_id;
  const char *content_name;
  const char *icon_url;
} meta_info_t;

typedef struct {
  language_t   languages[NUM_LANGUAGES];
  scenario_id_t playgo_scenario_ids[NUM_IDS];
  content_id_t  content_ids[NUM_IDS];
  long          unknown[810];
} playgo_info_t;

extern int sceAppInstUtilInitialize(void);
extern int sceAppInstUtilInstallByPackage(meta_info_t   *meta,
                                          pkg_info_t    *pkg_info,
                                          playgo_info_t *playgo);


/* ── timed sceAppInstUtilInitialize ──────────────────────────────────── */
static volatile int g_init_done = 0;
static volatile int g_init_rc   = -1;

static void *
init_thread(void *arg)
{
    (void)arg;
    g_init_rc   = sceAppInstUtilInitialize();
    g_init_done = 1;
    return NULL;
}

static int
timed_init(void)
{
    pthread_t tid;
    if (pthread_create(&tid, NULL, init_thread, NULL) != 0)
        return -1;
    pthread_detach(tid);

    for (int i = 0; i < INIT_TIMEOUT * 10; i++) {
        if (g_init_done)
            return g_init_rc;
        struct timespec ts = {0, 100000000}; /* 100 ms */
        nanosleep(&ts, NULL);
    }
    /* Timed out — init thread is stuck.  Return a sentinel so callers
     * can report "init:timeout" rather than silently hanging. */
    return -0xDEAD;
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(void) {
  /* kstuff spawns after all other payloads.  We sleep up to 25 s in
   * 500 ms steps so we catch kstuff's kernel patches before initialising
   * AppInstUtil.  On FW < 11.00 init succeeds once patches are live; on
   * FW 11.00+ it still fails (no SYSTEM_AUTHID in the spawned process)
   * and Arsenal's in-process path takes over from the caller. */
  {
    struct timespec ts = {0, 500000000}; /* 500 ms */
    for (int i = 0; i < 50; i++) nanosleep(&ts, NULL); /* 25 s total */
  }
  fprintf(stderr, "[dpi] startup wait done, calling sceAppInstUtilInitialize\n");
  int init_rc = timed_init();

  fprintf(stderr, "[dpi] sceAppInstUtilInitialize rc=0x%x%s\n",
          (unsigned)init_rc, init_rc == 0 ? " (ok)" : " (fallback mode)");

  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) { fprintf(stderr, "[dpi] socket failed\n"); return 1; }

  int opt = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in sa = {0};
  sa.sin_family      = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sa.sin_port        = htons(DPI_PORT);

  if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    fprintf(stderr, "[dpi] bind port %d failed\n", DPI_PORT);
    close(srv); return 1;
  }
  if (listen(srv, 4) < 0) { close(srv); return 1; }
  fprintf(stderr, "[dpi] listening on 127.0.0.1:%d\n", DPI_PORT);

  for (;;) {
    int cl = accept(srv, NULL, NULL);
    if (cl < 0) continue;

    char url[2048] = {0};
    ssize_t n = recv(cl, url, sizeof(url) - 1, 0);
    if (n <= 0) { close(cl); continue; }
    url[n] = '\0';

    for (ssize_t i = n - 1; i >= 0 && (url[i] == '\n' || url[i] == '\r' ||
                                         url[i] == ' '); i--)
      url[i] = '\0';

    if (init_rc != 0) {
      /* Retry once: IPMI service may have become available since startup. */
      fprintf(stderr, "[dpi] retrying init (prev rc=0x%x)\n", (unsigned)init_rc);
      init_rc = timed_init();
    }
    if (init_rc != 0) {
      char err[64];
      if (init_rc == -0xDEAD)
          snprintf(err, sizeof(err), "error:init:timeout");
      else
          snprintf(err, sizeof(err), "error:init:0x%08X", (unsigned)init_rc);
      fprintf(stderr, "[dpi] init failed, sending error: %s\n", err);
      send(cl, err, strlen(err), 0);
      close(cl);
      continue;
    }

    meta_info_t   meta   = {0};
    pkg_info_t    pkg    = {0};
    playgo_info_t playgo = {0};

    meta.uri                 = url;
    meta.ex_uri              = "";
    meta.playgo_scenario_id  = "";
    meta.content_id          = "";
    meta.content_name        = "Elf Arsenal DPI";
    meta.icon_url            = "";

    int rc = sceAppInstUtilInstallByPackage(&meta, &pkg, &playgo);
    if (rc == 0) {
      send(cl, "ok", 2, 0);
    } else {
      char err[64];
      snprintf(err, sizeof(err), "error:0x%08X", (unsigned)rc);
      send(cl, err, strlen(err), 0);
    }
    close(cl);
  }

  close(srv);
  return 0;
}
