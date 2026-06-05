
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "activity.h"
#include "dashboards.h"
#include "kmonitor.h"
#include "notif_inbox.h"
#include "titleid.h"


#define KSTUFF_TOGGLE_OFFSET     14
#define KSTUFF_TOGGLE_ENABLED    0xdeb7u
#define KSTUFF_TOGGLE_DISABLED   0xffffu

/* Defaults. Overridable at runtime via kmonitor_set_delays(). */
#define KMON_DEFAULT_PAUSE_SECONDS  25
#define KMON_DEFAULT_RESUME_SECONDS 10
#define KMON_MAX_DELAY_SECONDS      3600

#define KLOG_HOST "127.0.0.1"
#define KLOG_PORT 3232

static atomic_int g_pause_delay_seconds  = KMON_DEFAULT_PAUSE_SECONDS;
static atomic_int g_resume_delay_seconds = KMON_DEFAULT_RESUME_SECONDS;
static atomic_int g_auto_toggle_enabled  = 0; /* klog-driven auto-toggle — off by default; user opts in via Settings */

#define TITLE_LEN 16
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static char    g_active_title[TITLE_LEN];
static int     g_pause_armed;
static int     g_resume_armed;
static struct timespec g_pause_at;
static struct timespec g_resume_at;
static int     g_kstuff_paused_by_us;


typedef struct notify_request {
  char useless[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);


static void
kmon_notify(const char *fmt, ...) {
  notify_request_t req = {0};
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(req.message, sizeof(req.message), fmt, ap);
  va_end(ap);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}


/*  kstuff sysentvec resolver — mirrors ShadowMountPlus/sm_kstuff.c       */

static int
resolve_kstuff_sysentvecs(intptr_t *ps5_out, intptr_t *ps4_out) {
  intptr_t base = KERNEL_ADDRESS_DATA_BASE;

  switch(kernel_get_fw_version() & 0xffff0000u) {
  case 0x3000000: case 0x3100000: case 0x3200000: case 0x3210000:
    *ps5_out = base + 0xca0cd8; *ps4_out = base + 0xca0e50; return 1;
  case 0x4000000: case 0x4020000: case 0x4030000: case 0x4500000:
  case 0x4510000:
    *ps5_out = base + 0xd11bb8; *ps4_out = base + 0xd11d30; return 1;
  case 0x5000000: case 0x5020000: case 0x5100000: case 0x5500000:
    *ps5_out = base + 0xe00be8; *ps4_out = base + 0xe00d60; return 1;
  case 0x6000000: case 0x6020000: case 0x6500000:
    *ps5_out = base + 0xe210a8; *ps4_out = base + 0xe21220; return 1;
  case 0x7000000: case 0x7010000:
    *ps5_out = base + 0xe21ab8; *ps4_out = base + 0xe21c30; return 1;
  case 0x7200000: case 0x7400000: case 0x7600000: case 0x7610000:
    *ps5_out = base + 0xe21b78; *ps4_out = base + 0xe21cf0; return 1;
  case 0x8000000: case 0x8200000: case 0x8400000: case 0x8600000:
    *ps5_out = base + 0xe21ca8; *ps4_out = base + 0xe21e20; return 1;
  case 0x9000000: case 0x9050000: case 0x9200000: case 0x9400000:
  case 0x9600000:
    *ps5_out = base + 0xdba648; *ps4_out = base + 0xdba7c0; return 1;
  case 0x10000000: case 0x10010000: case 0x10200000: case 0x10400000:
  case 0x10600000:
    *ps5_out = base + 0xdba6d8; *ps4_out = base + 0xdba850; return 1;
  case 0x11000000: case 0x11200000:
    *ps5_out = base + 0xdcbc78; *ps4_out = base + 0xdcbdf0; return 1;
  case 0x11400000: case 0x11600000:
    *ps5_out = base + 0xdcbc98; *ps4_out = base + 0xdcbe10; return 1;
  case 0x12000000: case 0x12020000: case 0x12200000: case 0x12400000:
  case 0x12600000: case 0x12700000:
    *ps5_out = base + 0xdcc978; *ps4_out = base + 0xdccaf0; return 1;
  default:
    return 0;
  }
}


static int
kstuff_set_enabled(int enabled) {
  intptr_t ps5 = 0, ps4 = 0;
  uint16_t v = enabled ? KSTUFF_TOGGLE_ENABLED : KSTUFF_TOGGLE_DISABLED;
  static atomic_int last_notified = -1; /* -1=unset, 0=paused, 1=enabled */

  if(!resolve_kstuff_sysentvecs(&ps5, &ps4)) {
    return -1;
  }
  kernel_setshort(ps5 + KSTUFF_TOGGLE_OFFSET, v);
  kernel_setshort(ps4 + KSTUFF_TOGGLE_OFFSET, v);

  printf("kmonitor: kstuff %s\n", enabled ? "enabled" : "paused");

  int prev = atomic_exchange(&last_notified, enabled ? 1 : 0);
  if(prev != (enabled ? 1 : 0)) {
    kmon_notify(enabled ? "kstuff resumed" : "kstuff paused for game");
  }
  return 0;
}


/* Public settings API. */

int
kmonitor_kstuff_supported(void) {
  intptr_t ps5 = 0, ps4 = 0;
  return resolve_kstuff_sysentvecs(&ps5, &ps4) ? 1 : 0;
}

int
kmonitor_kstuff_is_enabled(void) {
  intptr_t ps5 = 0, ps4 = 0;
  if(!resolve_kstuff_sysentvecs(&ps5, &ps4)) {
    return -1;
  }
  uint16_t v5 = (uint16_t)kernel_getshort(ps5 + KSTUFF_TOGGLE_OFFSET);
  uint16_t v4 = (uint16_t)kernel_getshort(ps4 + KSTUFF_TOGGLE_OFFSET);
  if(v5 == KSTUFF_TOGGLE_DISABLED || v4 == KSTUFF_TOGGLE_DISABLED) {
    return 0;
  }
  return 1;
}

int
kmonitor_kstuff_set(int on) {
  return kstuff_set_enabled(on ? 1 : 0);
}

void
kmonitor_get_delays(int *pause_seconds, int *resume_seconds) {
  if(pause_seconds)  *pause_seconds  = atomic_load(&g_pause_delay_seconds);
  if(resume_seconds) *resume_seconds = atomic_load(&g_resume_delay_seconds);
}

int
kmonitor_auto_toggle_enabled(void) {
  return atomic_load(&g_auto_toggle_enabled);
}

void
kmonitor_set_auto_toggle(int on) {
  int prev = atomic_exchange(&g_auto_toggle_enabled, on ? 1 : 0);
  printf("kmonitor: auto-toggle %s\n", on ? "enabled" : "disabled");

  if(prev != 0 && on == 0) {
    pthread_mutex_lock(&g_lock);
    g_active_title[0] = 0;
    g_pause_armed     = 0;
    g_resume_armed    = 0;
    int was_paused    = g_kstuff_paused_by_us;
    g_kstuff_paused_by_us = 0;
    pthread_mutex_unlock(&g_lock);
    if(was_paused || kmonitor_kstuff_is_enabled() == 0) {
      kstuff_set_enabled(1);
    }
  }
  /* Persist the toggle so it survives a redeploy. config_save() is a
     no-op on the very first call (config_load runs first at boot). */
  extern void config_save(void);
  config_save();
}

void
kmonitor_set_delays(int pause_seconds, int resume_seconds) {
  if(pause_seconds  < 0) pause_seconds  = 0;
  if(resume_seconds < 0) resume_seconds = 0;
  if(pause_seconds  > KMON_MAX_DELAY_SECONDS)  pause_seconds  = KMON_MAX_DELAY_SECONDS;
  if(resume_seconds > KMON_MAX_DELAY_SECONDS) resume_seconds = KMON_MAX_DELAY_SECONDS;
  atomic_store(&g_pause_delay_seconds,  pause_seconds);
  atomic_store(&g_resume_delay_seconds, resume_seconds);
  printf("kmonitor: delays updated pause=%ds resume=%ds\n",
         pause_seconds, resume_seconds);
  extern void config_save(void);
  config_save();
}


/* (Shared state moved to the top of the file so it's visible from
   kmonitor_set_auto_toggle().) */


static void
schedule_at(struct timespec *out, int seconds) {
  clock_gettime(CLOCK_REALTIME, out);
  out->tv_sec += seconds;
}


/*  Klog parsing                                                          */

static int
find_title_id(const char *line, char *out, size_t out_size) {
  if(out_size < 10) return 0;
  for(const char *p = line; *p; p++) {
    char c = *p;
    if(c != 'C' && c != 'P' && c != 'U' && c != 'S' &&
       c != 'c' && c != 'p' && c != 'u' && c != 's') continue;
    char norm[10];
    if(title_id_normalize(p, norm)) {
      memcpy(out, norm, 10);
      return 1;
    }
  }
  return 0;
}


static int
contains_ci(const char *hay, const char *needle) {
  size_t nl = strlen(needle);
  for(const char *p=hay; *p; p++) {
    if(!strncasecmp(p, needle, nl)) {
      return 1;
    }
  }
  return 0;
}


typedef enum { EVT_NONE, EVT_LAUNCH, EVT_EXIT } klog_event_t;

static int
strong_launch_marker(const char *line) {
  return strstr(line, "launchApp(")     != NULL ||
         strstr(line, "BigApp Launch")  != NULL ||
         strstr(line, "/system_ex/app/") != NULL ||
         strstr(line, "exec EXEC")      != NULL ||
         strstr(line, "EXEC /system_ex/app/") != NULL;
}

static int
strong_exit_marker(const char *line) {
  return strstr(line, "killApp(")          != NULL ||
         strstr(line, "AppExited")         != NULL ||
         strstr(line, "App.Exited")        != NULL ||
         strstr(line, "EndAppMount")       != NULL ||
         strstr(line, "deleting bigapp")   != NULL ||
         strstr(line, "AppShutdownByExit") != NULL;
}


static klog_event_t
classify_line(const char *line) {
  /* Strong markers first — they are unambiguous on PS5. */
  if(strong_exit_marker(line))   return EVT_EXIT;
  if(strong_launch_marker(line)) return EVT_LAUNCH;

  /* Loose keyword scan as a fallback. Exit signals first because
     shutdown lines often also include the launch path string. */
  static const char *exit_words[] = {
    "exit", "exited", "killed", "kill ", "terminated",
    "shutdown", "stopped", "destroyed", "closed", NULL,
  };
  static const char *launch_words[] = {
    "launch", "launched", "exec ", "execve", "started",
    "starting", "running", "open ", "opened", "spawn",
    "BIGAPP", "bigapp", NULL,
  };

  for(const char **w=exit_words; *w; w++) {
    if(contains_ci(line, *w)) return EVT_EXIT;
  }
  for(const char **w=launch_words; *w; w++) {
    if(contains_ci(line, *w)) return EVT_LAUNCH;
  }
  return EVT_NONE;
}


static void
scan_for_crash(const char *line) {
  static const char * const KEYWORDS[] = {
    "Abort Trap",       "abort trap",
    "Trap 6",
    "SIGABRT", "SIGSEGV", "SIGBUS", "SIGILL", "SIGFPE",
    "kernel panic", "Kernel panic", "panic:",
    "core dump", "coredump",
    "double free", "double-free",
    "heap corruption",
    "stack smashing", "stack canary",
    "use-after-free",
    "Bus error",
    NULL
  };

  const char *hit = NULL;
  for (const char * const *k = KEYWORDS; *k; k++) {
    const char *p = strstr(line, *k);
    if (p) { hit = p; break; }
  }
  if (!hit) return;

  /* Debounce: drop duplicates of the same trimmed line within a 5s
     window so a process that traps repeatedly only counts once. */
  static char  last_msg[256];
  static time_t last_at = 0;
  time_t now = time(NULL);

  /* Trim the leading "<NUM>" syslog priority + the timestamp prefix
     so the notification bell shows the meaningful tail of the line. */
  const char *body = line;
  if (*body == '<') {
    const char *gt = strchr(body, '>');
    if (gt) body = gt + 1;
  }
  while (*body == ' ' || *body == '\t') body++;
  /* Skip a "12:34:56 PM" or "12:34:56 " timestamp if present. */
  if (isdigit((unsigned char)body[0]) && isdigit((unsigned char)body[1]) &&
      body[2] == ':') {
    const char *sp = strchr(body, ' ');
    if (sp) {
      const char *next = sp + 1;
      if (next[0] == 'A' || next[0] == 'P') {
        const char *sp2 = strchr(next, ' ');
        if (sp2) body = sp2 + 1;
      } else {
        body = next;
      }
    }
  }

  char msg[240];
  snprintf(msg, sizeof(msg), "⚠ %s", body);
  /* Strip trailing CR/LF/whitespace. */
  size_t mn = strlen(msg);
  while (mn > 0 && (msg[mn-1] == '\n' || msg[mn-1] == '\r' ||
                    msg[mn-1] == ' '  || msg[mn-1] == '\t')) {
    msg[--mn] = 0;
  }

  if (now - last_at < 5 && !strcmp(last_msg, msg)) return;
  strncpy(last_msg, msg, sizeof(last_msg) - 1);
  last_msg[sizeof(last_msg) - 1] = 0;
  last_at = now;

  notif_inbox_push(msg);
}


static void
on_klog_line(const char *line) {
  /* Mirror every line into the dashboards klog ring so the /klog
     viewer page can show it. Cheap — single locked memcpy. */
  dashboards_klog_push(line);

  scan_for_crash(line);

  char title[TITLE_LEN];
  if(!find_title_id(line, title, sizeof(title))) {
    return;
  }

  klog_event_t evt = classify_line(line);
  if(evt == EVT_NONE) {
    return;
  }

  if(evt == EVT_LAUNCH)      activity_record_launch(title);
  else if(evt == EVT_EXIT)   activity_record_exit(title);

  if(!atomic_load(&g_auto_toggle_enabled)) {
    return;
  }

  /* Don't auto-toggle kstuff when Arsenal's own tile launches/exits. */
  if(strncmp(title, "PSPS69691", 9) == 0) {
    return;
  }

  pthread_mutex_lock(&g_lock);

  if(evt == EVT_LAUNCH) {
    if(strcmp(g_active_title, title) != 0) {
      int pause_secs  = atomic_load(&g_pause_delay_seconds);
      printf("kmonitor: launch detected: %s (pause in %ds)\n",
             title, pause_secs);
      strncpy(g_active_title, title, sizeof(g_active_title)-1);
      g_active_title[sizeof(g_active_title)-1] = 0;
      g_pause_armed = 1;
      g_resume_armed = 0;
      schedule_at(&g_pause_at, pause_secs);
      pthread_cond_broadcast(&g_cond);
    }
  } else if(evt == EVT_EXIT) {
    if(g_active_title[0] && !strcmp(g_active_title, title)) {
      int resume_secs = atomic_load(&g_resume_delay_seconds);
      printf("kmonitor: exit detected: %s (resume in %ds)\n",
             title, resume_secs);
      g_active_title[0] = 0;
      g_pause_armed = 0;
      g_resume_armed = 1;
      schedule_at(&g_resume_at, resume_secs);
      pthread_cond_broadcast(&g_cond);
    }
  }

  pthread_mutex_unlock(&g_lock);
}


/*  Threads                                                               */

static int
connect_klogsrv(void) {
  struct sockaddr_in addr;
  int fd;

  if((fd=socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    return -1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(KLOG_PORT);
  inet_pton(AF_INET, KLOG_HOST, &addr.sin_addr);

  if(connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}


static void*
klog_reader_thread(void *arg) {
  (void)arg;
  syscall(SYS_thr_set_name, -1, "kmon-reader");

  /* Klogsrv may not be ready immediately. */
  (void)0;
  for(;;) {
    int fd = connect_klogsrv();
    if(fd < 0) {
      sleep(2);
      continue;
    }

    char line[1024];
    size_t fill = 0;
    char buf[256];
    ssize_t n;

    printf("kmonitor: connected to klogsrv on %s:%d\n", KLOG_HOST, KLOG_PORT);

    while((n=read(fd, buf, sizeof(buf))) > 0) {
      for(ssize_t i=0; i<n; i++) {
        char c = buf[i];
        if(c == '\n' || c == '\r' || fill+1 >= sizeof(line)) {
          line[fill] = 0;
          if(fill > 0) {
            on_klog_line(line);
          }
          fill = 0;
        } else {
          line[fill++] = c;
        }
      }
    }

    close(fd);
    printf("kmonitor: klogsrv disconnected (%s), retrying...\n",
           strerror(errno));
    sleep(2);
  }
  return NULL;
}


static int
ts_lt(const struct timespec *a, const struct timespec *b) {
  if(a->tv_sec  != b->tv_sec)  return a->tv_sec  < b->tv_sec;
  return a->tv_nsec < b->tv_nsec;
}


static void*
kstuff_timer_thread(void *arg) {
  (void)arg;
  syscall(SYS_thr_set_name, -1, "kmon-timer");

  pthread_mutex_lock(&g_lock);
  for(;;) {
    int do_pause  = 0;
    int do_resume = 0;
    struct timespec now;
    struct timespec wait_until;
    int have_wait = 0;

    clock_gettime(CLOCK_REALTIME, &now);

    if(g_pause_armed) {
      if(!ts_lt(&now, &g_pause_at)) {
        do_pause = 1;
        g_pause_armed = 0;
      } else {
        wait_until = g_pause_at;
        have_wait = 1;
      }
    }
    if(!do_pause && g_resume_armed) {
      if(!ts_lt(&now, &g_resume_at)) {
        do_resume = 1;
        g_resume_armed = 0;
      } else {
        if(!have_wait || ts_lt(&g_resume_at, &wait_until)) {
          wait_until = g_resume_at;
          have_wait = 1;
        }
      }
    }

    if(do_pause) {
      pthread_mutex_unlock(&g_lock);
      if(kstuff_set_enabled(0) == 0) {
        pthread_mutex_lock(&g_lock);
        g_kstuff_paused_by_us = 1;
      } else {
        pthread_mutex_lock(&g_lock);
      }
      continue;
    }
    if(do_resume) {
      int was_paused = g_kstuff_paused_by_us;
      pthread_mutex_unlock(&g_lock);
      if(was_paused) {
        kstuff_set_enabled(1);
      }
      pthread_mutex_lock(&g_lock);
      g_kstuff_paused_by_us = 0;
      continue;
    }

    if(have_wait) {
      pthread_cond_timedwait(&g_cond, &g_lock, &wait_until);
    } else {
      pthread_cond_wait(&g_cond, &g_lock);
    }
  }
  pthread_mutex_unlock(&g_lock);
  return NULL;
}


void
kmonitor_start(void) {
  pthread_t t1, t2;
  pthread_attr_t a;
  intptr_t ps5 = 0, ps4 = 0;

  pthread_attr_init(&a);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);

  /* klog reader only needs klogsrv on TCP 3232 — start it unconditionally
     so the klog buffer and /klog viewer always work regardless of kstuff. */
  pthread_create(&t1, &a, klog_reader_thread, NULL);

  if(!resolve_kstuff_sysentvecs(&ps5, &ps4)) {
    fprintf(stderr,
            "kmonitor: unsupported firmware (0x%08x), kstuff toggle off\n",
            kernel_get_fw_version());
    kmon_notify("Elf Arsenal: kstuff kernel patches unsupported on this "
                "firmware — everything else still works (PS5 Linux loader "
                "needs FW ≤ 6.02; that's a separate gate)");
    pthread_attr_destroy(&a);
    return;
  }

  pthread_create(&t2, &a, kstuff_timer_thread, NULL);
  pthread_attr_destroy(&a);

  printf("kmonitor: started (pause +%ds, resume +%ds)\n",
         atomic_load(&g_pause_delay_seconds),
         atomic_load(&g_resume_delay_seconds));
}
