
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "smp_meta.h"
#include "sys.h"


#define APP_DIR     "/user/app"
#define APPMETA_DIR "/user/appmeta"

#define POLL_SECONDS_DEFAULT  30
#define POLL_SECONDS_MIN       5
#define POLL_SECONDS_MAX     600


static const char *META_FILES[] = {
  "icon0.png",   /* home-screen tile */
  "pic0.png",    /* loading screen art */
  "pic1.png",    /* full-bleed background */
  "icon1.png",
  "snd0.at9",    /* boot jingle, harmless to omit but we copy it anyway */
  NULL,
};

#define PARAM_JSON  "sce_sys/param.json"



static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int       g_poll_seconds   = POLL_SECONDS_DEFAULT;
static atomic_int       g_run_now_flag   = 0;
static smp_meta_stats_t g_stats          = {0};
static int              g_thread_started = 0;



static int
title_id_looks_valid(const char *name) {
  size_t n = strlen(name);
  if(n < 8 || n > 12) return 0;
  for(size_t i = 0; i < n; i++) {
    char c = name[i];
    if(!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return 0;
  }
  return 1;
}


static int
file_exists_nonempty(const char *path) {
  struct stat st;
  if(stat(path, &st) != 0) return 0;
  return st.st_size > 0;
}


/* Recursively chmod 0777 every file and directory under path. */
static void
chmod_recursive(const char *path) {
  chmod(path, 0777);
  DIR *d = opendir(path);
  if(!d) return;
  struct dirent *e;
  while((e = readdir(d))) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char child[512];
    snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
    struct stat st;
    if(lstat(child, &st) != 0) continue;
    chmod(child, 0777);
    if(S_ISDIR(st.st_mode)) chmod_recursive(child);
  }
  closedir(d);
}


/* mkdir -p of a single path. Ignores EEXIST. */
static int
mkdir_one(const char *path) {
  if(mkdir(path, 0755) == 0) return 0;
  if(errno == EEXIST) return 0;
  return -1;
}


static int
copy_file(const char *src, const char *dst) {
  int sfd = -1, dfd = -1;
  uint8_t buf[16384];
  ssize_t n;
  int rc = -1;

  if((sfd = open(src, O_RDONLY)) < 0) goto done;
  if((dfd = open(dst, O_CREAT | O_WRONLY | O_TRUNC, 0644)) < 0) goto done;

  while((n = read(sfd, buf, sizeof(buf))) > 0) {
    ssize_t off = 0;
    while(off < n) {
      ssize_t w = write(dfd, buf + off, n - off);
      if(w <= 0) goto done;
      off += w;
    }
  }
  if(n < 0) goto done;
  rc = 0;

done:
  if(sfd >= 0) close(sfd);
  if(dfd >= 0) close(dfd);
  if(rc != 0) unlink(dst);   /* don't leave a half-written file behind */
  return rc;
}


static int
heal_one_title(const char *title_id) {
  char appmeta_dir[256];
  char src_meta_dir[256];
  char src_path[384];
  char dst_path[384];
  int  icon_ok = 0;
  int  any_pic_ok = 0;
  int  json_ok = 0;

  snprintf(appmeta_dir,  sizeof(appmeta_dir),
           "%s/%s", APPMETA_DIR, title_id);
  snprintf(src_meta_dir, sizeof(src_meta_dir),
           "%s/%s/sce_sys", APP_DIR, title_id);

  if(mkdir_one(APPMETA_DIR) < 0)  return 0;
  if(mkdir_one(appmeta_dir) < 0)  return 0;

  for(int i = 0; META_FILES[i]; i++) {
    snprintf(dst_path, sizeof(dst_path),
             "%s/%s", appmeta_dir, META_FILES[i]);

    if(file_exists_nonempty(dst_path)) {
      if(!strcmp(META_FILES[i], "icon0.png")) icon_ok = 1;
      if(!strncmp(META_FILES[i], "pic", 3))   any_pic_ok = 1;
      continue;
    }

    /* Try sce_sys/<file> first, then bare <file> at the app root. */
    snprintf(src_path, sizeof(src_path),
             "%s/%s", src_meta_dir, META_FILES[i]);
    int copied = 0;
    if(file_exists_nonempty(src_path) &&
       copy_file(src_path, dst_path) == 0) {
      copied = 1;
    } else {
      snprintf(src_path, sizeof(src_path),
               "%s/%s/%s", APP_DIR, title_id, META_FILES[i]);
      if(file_exists_nonempty(src_path) &&
         copy_file(src_path, dst_path) == 0) {
        copied = 1;
      }
    }

    if(copied) {
      pthread_mutex_lock(&g_lock);
      if(!strcmp(META_FILES[i], "icon0.png")) {
        g_stats.icons_healed++;
        icon_ok = 1;
      } else if(!strncmp(META_FILES[i], "pic", 3) ||
                !strncmp(META_FILES[i], "icon", 4) ||
                !strcmp(META_FILES[i], "snd0.at9")) {
        g_stats.pics_healed++;
        any_pic_ok = 1;
      }
      pthread_mutex_unlock(&g_lock);
    }
  }

  /* param.json carries the visible game name; without it the tile
     shows "Unknown Title". */
  snprintf(src_path, sizeof(src_path),
           "%s/%s/%s", APP_DIR, title_id, PARAM_JSON);
  snprintf(dst_path, sizeof(dst_path),
           "%s/param.json", appmeta_dir);
  if(file_exists_nonempty(dst_path)) {
    json_ok = 1;
  } else if(file_exists_nonempty(src_path) &&
            copy_file(src_path, dst_path) == 0) {
    pthread_mutex_lock(&g_lock);
    g_stats.json_healed++;
    pthread_mutex_unlock(&g_lock);
    json_ok = 1;
  }

  (void)any_pic_ok;
  (void)json_ok;
  return icon_ok;
}


/* One full sweep over /user/app. */
static void
sweep_once(void) {

  DIR *d = opendir(APP_DIR);
  if(!d) {
    /* /user/app doesn't exist — pre-jailbreak boot or no games
       installed. Not an error. */
    return;
  }

  int local_scanned = 0;
  int local_missing = 0;
  char last_missing[64] = "";

  struct dirent *e;
  while((e = readdir(d))) {
    if(e->d_name[0] == '.')                      continue;
    if(!title_id_looks_valid(e->d_name))         continue;

    /* Skip non-directories. d_type may be DT_UNKNOWN on some FSes. */
    if(e->d_type != DT_DIR && e->d_type != DT_UNKNOWN) continue;
    if(e->d_type == DT_UNKNOWN) {
      char probe[256];
      snprintf(probe, sizeof(probe), "%s/%s", APP_DIR, e->d_name);
      struct stat st;
      if(stat(probe, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    }

    local_scanned++;

    char app_path[256];
    snprintf(app_path, sizeof(app_path), "%s/%s", APP_DIR, e->d_name);
    chmod_recursive(app_path);

    int healthy = heal_one_title(e->d_name);
    if(!healthy) {
      local_missing++;
      strncpy(last_missing, e->d_name, sizeof(last_missing) - 1);
      last_missing[sizeof(last_missing) - 1] = 0;
    }
  }
  closedir(d);

  pthread_mutex_lock(&g_lock);
  g_stats.games_scanned = local_scanned;
  g_stats.still_missing = local_missing;
  if(local_missing > 0)
    strncpy(g_stats.last_missing, last_missing,
            sizeof(g_stats.last_missing) - 1);
  else
    g_stats.last_missing[0] = 0;
  g_stats.last_run_unix = (uint64_t)time(NULL);
  pthread_mutex_unlock(&g_lock);
}



static void *
worker_thread_fn(void *arg) {
  (void)arg;
  syscall(SYS_thr_set_name, -1, "smp-meta");

  pthread_mutex_lock(&g_lock);
  g_stats.running = 1;
  pthread_mutex_unlock(&g_lock);

  while(1) {
    if(atomic_load(&g_run_now_flag)) {
      atomic_store(&g_run_now_flag, 0);
      sweep_once();
      sys_refresh_shellui();
    }
    sleep(1);
  }
  return NULL;
}



void
smp_meta_init(void) {
  pthread_mutex_lock(&g_lock);
  if(g_thread_started) {
    pthread_mutex_unlock(&g_lock);
    return;
  }
  g_thread_started = 1;
  pthread_mutex_unlock(&g_lock);

  pthread_t t;
  if(pthread_create(&t, NULL, worker_thread_fn, NULL) != 0) {
    perror("smp_meta: pthread_create");
    pthread_mutex_lock(&g_lock);
    g_thread_started = 0;
    pthread_mutex_unlock(&g_lock);
    return;
  }
  pthread_detach(t);
}


void
smp_meta_get_stats(smp_meta_stats_t *out) {
  if(!out) return;
  pthread_mutex_lock(&g_lock);
  *out = g_stats;
  out->poll_seconds = atomic_load(&g_poll_seconds);
  pthread_mutex_unlock(&g_lock);
}


int
smp_meta_run_now(void) {
  atomic_store(&g_run_now_flag, 1);
  return 0;
}


int
smp_meta_set_poll_seconds(int seconds) {
  if(seconds < POLL_SECONDS_MIN) seconds = POLL_SECONDS_MIN;
  if(seconds > POLL_SECONDS_MAX) seconds = POLL_SECONDS_MAX;
  atomic_store(&g_poll_seconds, seconds);
  return seconds;
}


int
smp_meta_get_poll_seconds(void) {
  return atomic_load(&g_poll_seconds);
}
