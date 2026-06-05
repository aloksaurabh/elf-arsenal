/* Copyright (C) 2024 John Törnblom

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

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>

#include <microhttpd.h>

#include "activity.h"
#include "activitydb.h"
#include "appdb.h"
#include "tmdb.h"
#include "asset.h"
#include "avatar.h"
#include "cheats.h"
#include "linux_loader.h"
#include "dashboards.h"
#include "fan.h"
#include "fs.h"
#include "homebrew.h"
#include "kmonitor.h"
#include "kstuff_updater.h"
#include "smp_updater.h"
#include "np.h"
#include "backup.h"
#include "fpkg_db.h"
#include "offline_pack.h"
#include "sdk_changer.h"
#include "notif_inbox.h"
#include "garlic.h"
#include "offact.h"
#include "y2jb_updater.h"
#include "dumper.h"
#include "transfer.h"
#include "translate.h"
#include "drive_sensors.h"
#include "mdns.h"
#include "smb.h"
#include "sys.h"
#include "ps5/http.h"
#include "ps5/notify.h"
#include "version.h"
#include "websrv.h"


typedef struct post_data {
  char *key;
  uint8_t *val;
  size_t len;
  struct post_data *next;
} post_data_t;


typedef struct post_request {
  struct MHD_PostProcessor* pp;
  post_data_t* data;
  void* cheats_state;       /* opaque per-connection state for /api/cheats/upload */
  void* avatar_upload_state; /* opaque per-connection state for /api/avatar/upload */
  void* pkg_upload_state;    /* opaque per-connection state for /api/homebrew/install-pkg-upload */
  void* fs_upload_state;     /* opaque per-connection state for /api/fs/upload */
  void* payload_upload_state;/* opaque per-connection state for /api/payloads/upload */
  void* xml_upload_state;    /* opaque per-connection state for /api/xmlpatches/upload */
} post_request_t;


static post_data_t*
post_data_get(post_data_t* data, const char* key) {
  if(!data) {
    return 0;
  }

  if(!strcmp(key, data->key)) {
    return data;
  }

  return post_data_get(data->next, key);
}


static const char*
post_data_val(post_data_t* data, const char* key) {
  data = post_data_get(data, key);
  return data ? (const char*)data->val : 0;
}


static enum MHD_Result
post_iterator(void *cls, enum MHD_ValueKind kind, const char *key,
               const char *filename, const char *mime, const char *encoding,
               const char *value, uint64_t off, size_t size) {
  post_request_t *req = cls;
  post_data_t *data = post_data_get(req->data, key);

  if(data) {
    data->val = realloc(data->val, off+size+1);
  } else {
    data = malloc(sizeof(post_data_t));
    data->next = req->data;
    data->key = strdup(key);
    data->val = malloc(off+size+1);
    data->len = 0;
    req->data = data;
  }

  memcpy(data->val+off, value, size);
  data->val[off+size] = 0;
  data->len += size;

  return MHD_YES;
}


enum MHD_Result
websrv_queue_response(struct MHD_Connection *conn, unsigned int status,
		      struct MHD_Response *resp) {
  MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN,
  			  "*");

  return MHD_queue_response(conn, status, resp);
}



static enum MHD_Result
version_request(struct MHD_Connection *conn) {
  size_t size = strlen(PAGE_VERSION);
  enum MHD_Result ret = MHD_NO;
  struct MHD_Response *resp;
  void* data = PAGE_VERSION;

  if((resp=MHD_create_response_from_buffer(size, data,
					   MHD_RESPMEM_PERSISTENT))) {
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE,
                            "application/json");
    ret = websrv_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
  }

  return ret;
}


extern int  cheats_engine_enabled(void);
extern void cheats_engine_set_enabled(int on);
extern int  cheats_game_running(void);

static enum MHD_Result
state_request(struct MHD_Connection *conn) {
  enum MHD_Result ret = MHD_NO;
  struct MHD_Response *resp;
  const char *auto_arg;
  const char *kstuff_arg;
  const char *pause_arg;
  const char *resume_arg;
  const char *cheats_arg;
  const char *backpork_arg;
  const char *lapyjb_arg;
  const char *nanodns_arg;
  const char *tile_autoinstall_arg;
  const char *klogsrv_arg;
  const char *trophy_all_arg;
  const char *trophy_uds_arg;
  int pause_secs = 25;
  int resume_secs = 10;
  int kstuff_supported;
  int kstuff_state;
  int auto_state;
  int cheats_state;
  int game_running;
  int backpork_state;
  int lapyjb_state;
  int lapyjb_enabled_state;
  int nanodns_state;
  int tile_autoinstall_state;
  char body[1200];
  size_t body_len;

  auto_arg     = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "auto");
  kstuff_arg   = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "kstuff");
  pause_arg    = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "pause");
  resume_arg   = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "resume");
  cheats_arg   = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "cheats");
  backpork_arg = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "backpork");
  lapyjb_arg   = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "lapyjb");
  nanodns_arg  = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "nanodns");
  tile_autoinstall_arg =
                 MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "tileAutoinstall");
  klogsrv_arg     = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "klogsrv");
  trophy_all_arg  = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "trophyAll");
  trophy_uds_arg  = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "trophyUds");

  if(auto_arg) {
    kmonitor_set_auto_toggle(strcmp(auto_arg, "0") != 0);
  }
  if(kstuff_arg) {
    kmonitor_kstuff_set(strcmp(kstuff_arg, "0") != 0);
  }
  if(cheats_arg) {
    int want_cheats = strcmp(cheats_arg, "0") != 0;
    if(!want_cheats || cheats_game_running()) {
      cheats_engine_set_enabled(want_cheats);
    }
  }
  if(backpork_arg) {
    sys_backpork_set_enabled(strcmp(backpork_arg, "0") != 0);
  }
  if(lapyjb_arg) {
    sys_lapyjb_set_enabled(strcmp(lapyjb_arg, "0") != 0);
  }
  {
    const char *cr_arg = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "cheatrunner");
    if(cr_arg) sys_cheatrunner_set_enabled(strcmp(cr_arg, "0") != 0);
  }
  {
    const char *fg_arg = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "fpkgguard");
    if(fg_arg) sys_fpkgguard_set_enabled(strcmp(fg_arg, "0") != 0);
  }
  if(nanodns_arg) {
    sys_nanodns_set_enabled(strcmp(nanodns_arg, "0") != 0);
  }
  if(tile_autoinstall_arg) {
    homebrew_tile_autoinstall_set_enabled(strcmp(tile_autoinstall_arg, "0") != 0);
  }
  if(klogsrv_arg)    sys_klogsrv_set_enabled(strcmp(klogsrv_arg, "0") != 0);
  {
    const char *dpiv2_arg = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "dpiv2");
    if(dpiv2_arg) sys_dpiv2_set_enabled(strcmp(dpiv2_arg, "0") != 0);
  }
  if(trophy_all_arg) sys_trophy_all_set_enabled(strcmp(trophy_all_arg, "0") != 0);
  if(trophy_uds_arg) sys_trophy_uds_set_enabled(strcmp(trophy_uds_arg, "0") != 0);
  {
    const char *patches_arg = MHD_lookup_connection_value(conn,
                                MHD_GET_ARGUMENT_KIND, "patchesAuto");
    if(patches_arg) cheats_patches_auto_set_enabled(strcmp(patches_arg, "0") != 0);
  }
  {
    const char *backup_arg = MHD_lookup_connection_value(conn,
                                MHD_GET_ARGUMENT_KIND, "backup");
    if(backup_arg) {
      backup_set_enabled(strcmp(backup_arg, "0") != 0);
      extern void config_save(void);
      config_save();
    }
  }

  if(pause_arg || resume_arg) {
    int cur_pause = 25, cur_resume = 10;
    kmonitor_get_delays(&cur_pause, &cur_resume);
    int new_pause  = pause_arg  ? atoi(pause_arg)  : cur_pause;
    int new_resume = resume_arg ? atoi(resume_arg) : cur_resume;
    kmonitor_set_delays(new_pause, new_resume);
  }

  kmonitor_get_delays(&pause_secs, &resume_secs);
  kstuff_supported = kmonitor_kstuff_supported();
  kstuff_state     = kmonitor_kstuff_is_enabled();
  auto_state       = kmonitor_auto_toggle_enabled();
  cheats_state     = cheats_engine_enabled();
  game_running     = cheats_game_running();
  backpork_state   = sys_backpork_is_running();
  lapyjb_state         = sys_lapyjb_is_running();
  lapyjb_enabled_state = sys_lapyjb_get_enabled();
  nanodns_state    = sys_nanodns_is_running();
  tile_autoinstall_state = homebrew_tile_autoinstall_enabled();

  char running_title[16] = {0};
  sys_get_running_title_id(running_title, sizeof running_title);

  body_len = (size_t)snprintf(body, sizeof(body),
    "{\"kstuffSupported\":%s,"
    "\"kstuffEnabled\":%s,"
    "\"autoToggleEnabled\":%s,"
    "\"cheatsEnabled\":%s,"
    "\"gameRunning\":%s,"
    "\"runningTitleId\":\"%s\","
    "\"backporkRunning\":%s,"
    "\"lapyjbRunning\":%s,"
    "\"lapyjbEnabled\":%s,"
    "\"lapyjbSupported\":%s,"
    "\"cheatrunnerEnabled\":%s,"
    "\"cheatrunnerRunning\":%s,"
    "\"fpkgGuardEnabled\":%s,"
    "\"fpkgGuardRunning\":%s,"
    "\"nanodnsRunning\":%s,"
    "\"tileAutoinstallEnabled\":%s,"
    "\"klogsrvRunning\":%s,"
    "\"trophyAllRunning\":%s,"
    "\"trophyAllEnabled\":%s,"
    "\"trophyUdsRunning\":%s,"
    "\"trophyUdsEnabled\":%s,"
    "\"patchesAutoEnabled\":%s,"
    "\"patchesLastMods\":%d,"
    "\"patchesTotalWrites\":%d,"
    "\"backupEnabled\":%s,"
    "\"dpiv2Enabled\":%s,"
    "\"dpiv2Running\":%s,"
    "\"pauseAfterSeconds\":%d,"
    "\"resumeAfterSeconds\":%d}",
    kstuff_supported ? "true" : "false",
    (kstuff_state == 1) ? "true" : "false",
    auto_state ? "true" : "false",
    cheats_state ? "true" : "false",
    game_running ? "true" : "false",
    running_title,
    backpork_state ? "true" : "false",
    lapyjb_state ? "true" : "false",
    lapyjb_enabled_state ? "true" : "false",
    "true",
    sys_cheatrunner_get_enabled() ? "true" : "false",
    sys_cheatrunner_is_running() ? "true" : "false",
    sys_fpkgguard_get_enabled()  ? "true" : "false",
    sys_fpkgguard_is_running()   ? "true" : "false",
    nanodns_state ? "true" : "false",
    tile_autoinstall_state ? "true" : "false",
    sys_klogsrv_is_running()    ? "true" : "false",
    sys_trophy_all_is_running() ? "true" : "false",
    sys_trophy_all_get_enabled() ? "true" : "false",
    sys_trophy_uds_is_running() ? "true" : "false",
    sys_trophy_uds_get_enabled() ? "true" : "false",
    cheats_patches_auto_enabled() ? "true" : "false",
    cheats_patches_last_mod_count(),
    cheats_patches_total_writes(),
    backup_is_enabled() ? "true" : "false",
    sys_dpiv2_get_enabled() ? "true" : "false",
    sys_dpiv2_is_running()  ? "true" : "false",
    pause_secs, resume_secs);

  if((resp=MHD_create_response_from_buffer(body_len, body,
                                           MHD_RESPMEM_MUST_COPY))) {
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE,
                            "application/json");
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-cache");
    ret = websrv_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
  }

  return ret;
}


static enum MHD_Result
launch_request(struct MHD_Connection *conn) {
  enum MHD_Result ret = MHD_NO;
  struct MHD_Response *resp;
  const char* title_id;
  unsigned int status;
  const char *args;

  title_id = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "titleId");
  args = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "args");

  if(!title_id) {
    status = MHD_HTTP_BAD_REQUEST;
  } else {
    /* Re-launching the currently-running title kernel-panics the PS5.
     * Hot-swapping to a *different* title is fine (SceSystemService
     * handles it), so we only short-circuit on exact title-id match. */
    char running[16] = {0};
    if(sys_get_running_title_id(running, sizeof(running)) == 0 &&
       running[0] != 0 &&
       strcasecmp(running, title_id) == 0) {
      status = MHD_HTTP_CONFLICT;
    } else if(sys_launch_title(title_id, args)) {
      status = MHD_HTTP_SERVICE_UNAVAILABLE;
    } else {
      status = MHD_HTTP_OK;
    }
  }

  if((resp=MHD_create_response_from_buffer(0, "",
					   MHD_RESPMEM_PERSISTENT))) {
    ret = websrv_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
  }

  return ret;
}


/* URL-encode a Wikipedia path component (spaces → underscores). */
static void
url_encode_into(char *dst, size_t dst_sz, const char *src) {
  static const char hex[] = "0123456789ABCDEF";
  size_t o = 0;
  for(; *src && o + 4 < dst_sz; src++) {
    unsigned char c = (unsigned char)*src;
    if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
       (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      dst[o++] = c;
    } else if(c == ' ') {
      dst[o++] = '_';
    } else {
      dst[o++] = '%';
      dst[o++] = hex[c >> 4];
      dst[o++] = hex[c & 0xF];
    }
  }
  dst[o] = 0;
}

/* Strip ™ ® © (UTF-8), collapse whitespace, trim. */
static void
clean_game_name(char *dst, size_t dst_sz, const char *src) {
  size_t o = 0;
  int last_space = 1;
  for(; *src && o + 1 < dst_sz; ) {
    unsigned char c = (unsigned char)*src;
    if(c == 0xE2 && (unsigned char)src[1] == 0x84 &&
       ((unsigned char)src[2] == 0xA2 || (unsigned char)src[2] == 0xA0)) {
      src += 3; continue;
    }
    if(c == 0xC2 && ((unsigned char)src[1] == 0xAE ||
                     (unsigned char)src[1] == 0xA9)) {
      src += 2; continue;
    }
    if(c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if(!last_space && o + 1 < dst_sz) { dst[o++] = ' '; last_space = 1; }
      src++; continue;
    }
    dst[o++] = (char)c;
    last_space = 0;
    src++;
  }
  while(o > 0 && dst[o-1] == ' ') o--;
  dst[o] = 0;
}

/* Wikipedia OpenSearch: find canonical article title for a free-form query. */
static int
wiki_opensearch_title(const char *query, char *out, size_t out_sz) {
  char enc[512];
  size_t qo = 0;
  static const char hex[] = "0123456789ABCDEF";
  for(const char *s = query; *s && qo + 4 < sizeof enc; s++) {
    unsigned char c = (unsigned char)*s;
    if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
       (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      enc[qo++] = c;
    } else if(c == ' ') {
      enc[qo++] = '+';
    } else {
      enc[qo++] = '%';
      enc[qo++] = hex[c >> 4];
      enc[qo++] = hex[c & 0xF];
    }
  }
  enc[qo] = 0;
  char url[1024];
  snprintf(url, sizeof url,
           "https://en.wikipedia.org/w/api.php?action=opensearch"
           "&format=json&namespace=0&limit=1&redirects=resolve&search=%s", enc);
  size_t len = 0;
  uint8_t *body = http_get(url, &len);
  if(!body || len < 4) { if(body) free(body); return -1; }
  const char *p = (const char *)body;
  const char *end = p + len;
  const char *titles = NULL;
  int depth = 0;
  for(; p < end; p++) {
    if(*p == '[') { depth++; if(depth == 2) { titles = p + 1; break; } }
  }
  int rc = -1;
  if(titles && titles < end) {
    while(titles < end && *titles != '"') titles++;
    if(titles < end) {
      titles++;
      size_t o = 0;
      while(titles < end && *titles != '"' && o + 1 < out_sz) {
        if(*titles == '\\' && titles + 1 < end) titles++;
        out[o++] = *titles++;
      }
      if(o > 0) { out[o] = 0; rc = 0; }
    }
  }
  free(body);
  return rc;
}

static enum MHD_Result
gameinfo_request(struct MHD_Connection *conn) {
  const char *name = MHD_lookup_connection_value(
      conn, MHD_GET_ARGUMENT_KIND, "name");
  const char *title_id = MHD_lookup_connection_value(
      conn, MHD_GET_ARGUMENT_KIND, "titleId");
  if(!name || !*name) name = title_id;
  if(!name || !*name) {
    const char *b = "{\"error\":\"missing 'name' or 'titleId' param\"}";
    struct MHD_Response *r = MHD_create_response_from_buffer(
        strlen(b), (void*)b, MHD_RESPMEM_PERSISTENT);
    if(!r) return MHD_NO;
    MHD_add_response_header(r, "Content-Type", "application/json");
    enum MHD_Result rc = websrv_queue_response(conn, MHD_HTTP_BAD_REQUEST, r);
    MHD_destroy_response(r); return rc;
  }
  char cleaned[512]; clean_game_name(cleaned, sizeof cleaned, name);
  char enc[512];     url_encode_into(enc, sizeof enc, cleaned);
  char url[1024];
  snprintf(url, sizeof url,
           "https://en.wikipedia.org/api/rest_v1/page/summary/%s", enc);
  size_t body_len = 0;
  uint8_t *body = http_get(url, &body_len);
  if(!body || body_len == 0) {
    if(body) { free(body); body = NULL; body_len = 0; }
    char canonical[512];
    if(wiki_opensearch_title(cleaned, canonical, sizeof canonical) == 0 &&
       canonical[0] && strcmp(canonical, cleaned) != 0) {
      url_encode_into(enc, sizeof enc, canonical);
      snprintf(url, sizeof url,
               "https://en.wikipedia.org/api/rest_v1/page/summary/%s", enc);
      body = http_get(url, &body_len);
    }
  }
  if(!body || body_len == 0) {
    if(body) free(body);
    const char *fb = "{\"source\":\"none\",\"name\":\"\",\"error\":\"no wikipedia article found\"}";
    struct MHD_Response *r = MHD_create_response_from_buffer(
        strlen(fb), (void*)fb, MHD_RESPMEM_PERSISTENT);
    if(!r) return MHD_NO;
    MHD_add_response_header(r, "Content-Type", "application/json");
    MHD_add_response_header(r, "Access-Control-Allow-Origin", "*");
    enum MHD_Result rc = websrv_queue_response(conn, MHD_HTTP_OK, r);
    MHD_destroy_response(r); return rc;
  }
  struct MHD_Response *r = MHD_create_response_from_buffer(
      body_len, body, MHD_RESPMEM_MUST_FREE);
  if(!r) { free(body); return MHD_NO; }
  MHD_add_response_header(r, "Content-Type", "application/json");
  MHD_add_response_header(r, "Access-Control-Allow-Origin", "*");
  enum MHD_Result rc = websrv_queue_response(conn, MHD_HTTP_OK, r);
  MHD_destroy_response(r);
  return rc;
}


static enum MHD_Result
kill_request(struct MHD_Connection *conn) {
  const char *title_id = MHD_lookup_connection_value(
      conn, MHD_GET_ARGUMENT_KIND, "titleId");
  unsigned int status = sys_kill_title(title_id, 3000)
      ? MHD_HTTP_SERVICE_UNAVAILABLE : MHD_HTTP_OK;
  struct MHD_Response *resp = MHD_create_response_from_buffer(
      0, "", MHD_RESPMEM_PERSISTENT);
  if(!resp) return MHD_NO;
  enum MHD_Result ret = websrv_queue_response(conn, status, resp);
  MHD_destroy_response(resp);
  return ret;
}


static void *
power_thread_reboot(void *arg) {
  (void)arg;
  notify("Elf Arsenal: REBOOT in 5s\xe2\x80\xa6");
  sleep(5);
  sys_console_reboot();
  return NULL;
}

static void *
power_thread_off(void *arg) {
  (void)arg;
  notify("Elf Arsenal: SHUTDOWN in 5s\xe2\x80\xa6");
  sleep(5);
  sys_console_poweroff();
  return NULL;
}

static void *
power_thread_sleep(void *arg) {
  (void)arg;
  notify("Elf Arsenal: entering standby\xe2\x80\xa6");
  sleep(2);
  sys_console_sleep();
  return NULL;
}

/* Self-terminate the payload so its listening sockets free up immediately —
   lets `make deploy` rebind without a console reboot. */
static void *
power_thread_exit(void *arg) {
  (void)arg;
  notify("Elf Arsenal: exiting to free ports for redeploy\xe2\x80\xa6");
  sleep(1);
  kill(getpid(), SIGKILL);
  return NULL;
}

static enum MHD_Result
power_request(struct MHD_Connection *conn) {
  const char *action = MHD_lookup_connection_value(
      conn, MHD_GET_ARGUMENT_KIND, "action");
  unsigned int status = MHD_HTTP_OK;
  const char *body = "{\"ok\":true}";
  pthread_t t;
  if(action && !strcmp(action, "reboot")) {
    pthread_create(&t, NULL, power_thread_reboot, NULL);
    pthread_detach(t);
  } else if(action && (!strcmp(action, "shutdown") || !strcmp(action, "poweroff"))) {
    pthread_create(&t, NULL, power_thread_off, NULL);
    pthread_detach(t);
  } else if(action && (!strcmp(action, "sleep") || !strcmp(action, "standby"))) {
    pthread_create(&t, NULL, power_thread_sleep, NULL);
    pthread_detach(t);
  } else if(action && (!strcmp(action, "exit") || !strcmp(action, "kill"))) {
    pthread_create(&t, NULL, power_thread_exit, NULL);
    pthread_detach(t);
  } else if(action && (!strcmp(action, "refreshui") || !strcmp(action, "refresh"))) {
    body = sys_refresh_shellui() == 0
         ? "{\"ok\":true,\"refreshed\":true}"
         : "{\"ok\":false,\"error\":\"shell-UI process not found\"}";
  } else {
    status = MHD_HTTP_BAD_REQUEST;
    body = "{\"error\":\"action must be reboot, shutdown, sleep, exit, or refreshui\"}";
  }
  struct MHD_Response *resp = MHD_create_response_from_buffer(
      strlen(body), (void*)body, MHD_RESPMEM_PERSISTENT);
  if(!resp) return MHD_NO;
  MHD_add_response_header(resp, "Content-Type", "application/json");
  enum MHD_Result ret = websrv_queue_response(conn, status, resp);
  MHD_destroy_response(resp);
  return ret;
}


/* GET /api/fwspoof/apply?version=MM.mm — spoof the reported firmware version.
   "MM.mm" maps to 0xMMmm0000 (each part 2 hex digits, matching the PS5
   version display). Applies live via the fw-spoof payload; needs kstuff. */
static enum MHD_Result
fwspoof_request(struct MHD_Connection *conn) {
  const char *v = MHD_lookup_connection_value(
      conn, MHD_GET_ARGUMENT_KIND, "version");
  unsigned int status = MHD_HTTP_OK;
  char body[160];
  unsigned maj = 0, mn = 0;

  if(!v || sscanf(v, "%2x.%2x", &maj, &mn) < 1 || (maj == 0 && mn == 0)) {
    status = MHD_HTTP_BAD_REQUEST;
    snprintf(body, sizeof(body),
             "{\"ok\":false,\"error\":\"version must be MM.mm (e.g. 9.60)\"}");
  } else {
    uint32_t target = ((maj & 0xffu) << 24) | ((mn & 0xffu) << 16);
    if(sys_fw_spoof_run(target) == 0) {
      snprintf(body, sizeof(body),
               "{\"ok\":true,\"target\":\"0x%08x\"}", (unsigned)target);
    } else {
      status = MHD_HTTP_SERVICE_UNAVAILABLE;
      snprintf(body, sizeof(body),
               "{\"ok\":false,\"error\":\"spawn failed — is kstuff enabled?\"}");
    }
  }
  struct MHD_Response *resp = MHD_create_response_from_buffer(
      strlen(body), body, MHD_RESPMEM_MUST_COPY);
  if(!resp) return MHD_NO;
  MHD_add_response_header(resp, "Content-Type", "application/json");
  enum MHD_Result ret = websrv_queue_response(conn, status, resp);
  MHD_destroy_response(resp);
  return ret;
}


static enum MHD_Result
hbldr_request(struct MHD_Connection *conn) {
  int (*sys_launch)(const char*, const char*, const char*, const char*) = 0;
  enum MHD_Result ret = MHD_NO;
  struct MHD_Response *resp;
  const char* daemon;
  const char* path;
  const char *args;
  const char *pipe;
  const char *env;
  const char *cwd;
  int fd;

  path = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "path");
  args = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "args");
  env = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "env");
  pipe = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "pipe");
  cwd = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "cwd");
  daemon = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "daemon");

  if(daemon && strcmp(daemon, "0")) {
    sys_launch = sys_launch_daemon;
  } else {
    sys_launch = sys_launch_homebrew;
  }

  if(!path) {
    if((resp=MHD_create_response_from_buffer(0, "", MHD_RESPMEM_PERSISTENT))) {
      ret = websrv_queue_response(conn, MHD_HTTP_BAD_REQUEST, resp);
      MHD_destroy_response(resp);
    }
  } else if((fd=sys_launch(cwd, path, args, env)) < 0) {
    extern const char *hbldr_last_error(void);
    const char *why = hbldr_last_error();
    char json[512];
    int n = snprintf(json, sizeof(json),
        "{\"ok\":false,\"error\":\"%s\",\"path\":\"%s\"}",
        (why && *why) ? why : "launch failed (no diagnostic)",
        path ? path : "");
    char *out = malloc(n + 1);
    if(!out) {
      if((resp=MHD_create_response_from_buffer(0, "",
                                               MHD_RESPMEM_PERSISTENT))) {
        ret = websrv_queue_response(conn, MHD_HTTP_SERVICE_UNAVAILABLE, resp);
        MHD_destroy_response(resp);
      }
    } else {
      memcpy(out, json, n + 1);
      if((resp=MHD_create_response_from_buffer(n, out,
                                               MHD_RESPMEM_MUST_FREE))) {
        MHD_add_response_header(resp, "Content-Type", "application/json");
        ret = websrv_queue_response(conn, MHD_HTTP_SERVICE_UNAVAILABLE, resp);
        MHD_destroy_response(resp);
      } else {
        free(out);
      }
    }
  } else if(pipe && strcmp(pipe, "0")) {
    if((resp=MHD_create_response_from_pipe(fd))) {
      MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "text/x-log; charset=utf-8");
      MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-cache");
      ret = websrv_queue_response(conn, MHD_HTTP_OK, resp);
      MHD_destroy_response(resp);
    }
  } else {
    close(fd);
    if((resp=MHD_create_response_from_buffer(0, "", MHD_RESPMEM_PERSISTENT))) {
      ret = websrv_queue_response(conn, MHD_HTTP_OK, resp);
      MHD_destroy_response(resp);
    }
  }

  return ret;
}



static enum MHD_Result
elfldr_request(struct MHD_Connection *conn, post_data_t *data) {
  enum MHD_Result ret = MHD_NO;
  struct MHD_Response *resp;
  const char *args;
  const char *pipe;
  const char *env;
  const char *cwd;
  const char *uri;
  int fd = -1;

  if(!(args=MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "args"))) {
    args = post_data_val(data, "args");
  }
  if(!(env=MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "env"))) {
    env = post_data_val(data, "env");
  }
  if(!(pipe=MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "pipe"))) {
    pipe = post_data_val(data, "pipe");
  }
  if(!(cwd=MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "cwd"))) {
    cwd = post_data_val(data, "cwd");
  }

  if((uri=MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "elf"))) {
    fd = sys_launch_daemon(cwd, uri, args, env);
  } else if((data=post_data_get(data, "elf"))) {
    fd = sys_launch_payload(cwd, data->val, data->len, args, env);
  } else {
    return asset_request(conn, "/elfldr.html");
  }

  if(fd < 0) {
    if((resp=MHD_create_response_from_buffer(0, "", MHD_RESPMEM_PERSISTENT))) {
      ret = websrv_queue_response(conn, MHD_HTTP_BAD_REQUEST, resp);
      MHD_destroy_response(resp);
    }
    return ret;
  }

  if(pipe && strcmp(pipe, "0")) {
    if((resp=MHD_create_response_from_pipe(fd))) {
      MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "text/x-log; charset=utf-8");
      MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-cache");
      ret = websrv_queue_response(conn, MHD_HTTP_OK, resp);
      MHD_destroy_response(resp);
    }
  } else {
    close(fd);
    if((resp=MHD_create_response_from_buffer(0, "", MHD_RESPMEM_PERSISTENT))) {
      ret = websrv_queue_response(conn, MHD_HTTP_OK, resp);
      MHD_destroy_response(resp);
    }
  }

  return ret;
}


static enum MHD_Result
db_snap_request(struct MHD_Connection *conn, const char *url) {
  const char *body = NULL;
  char *freeable = NULL;
  unsigned int status = MHD_HTTP_OK;

  if(!strcmp(url, "/api/db/snapshot")) {
    int r = fpkg_db_full_snapshot();
    body = r == 0 ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"snapshot failed\"}";
    if(r != 0) status = MHD_HTTP_INTERNAL_SERVER_ERROR;
  } else if(!strncmp(url, "/api/db/restore", 15)) {
    const char *from = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "from");
    if(!from || !*from) {
      body = "{\"ok\":false,\"error\":\"missing 'from' parameter\"}";
      status = MHD_HTTP_BAD_REQUEST;
    } else {
      int r = fpkg_db_full_restore(from);
      if(r == 0) {
        sys_refresh_shellui();
        body = "{\"ok\":true}";
      } else {
        body = "{\"ok\":false,\"error\":\"restore failed or slot not found\"}";
        status = MHD_HTTP_INTERNAL_SERVER_ERROR;
      }
    }
  } else if(!strcmp(url, "/api/db/snapshots")) {
    freeable = fpkg_db_snapshot_list();
    body = freeable ? freeable : "[]";
  } else {
    body = "{\"ok\":false,\"error\":\"unknown endpoint\"}";
    status = MHD_HTTP_NOT_FOUND;
  }

  struct MHD_Response *resp = MHD_create_response_from_buffer(
      strlen(body), (void*)body, MHD_RESPMEM_MUST_COPY);
  enum MHD_Result ret = MHD_NO;
  if(resp) {
    MHD_add_response_header(resp, "Content-Type", "application/json");
    ret = websrv_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
  }
  free(freeable);
  return ret;
}


static enum MHD_Result
websrv_on_request(void *cls, struct MHD_Connection *conn,
                  const char *url, const char *method,
                  const char *version, const char *upload_data,
                  size_t *upload_data_size, void **con_cls) {
  post_request_t *req = *con_cls;
  enum MHD_Result ret = MHD_NO;

  if(strcmp(method, MHD_HTTP_METHOD_GET) &&
     strcmp(method, MHD_HTTP_METHOD_POST) &&
     strcmp(method, MHD_HTTP_METHOD_HEAD)) {
    return MHD_NO;
  }

  if(!req) {
    req = *con_cls = malloc(sizeof(post_request_t));
    req->pp = MHD_create_post_processor(conn, 0x1000, &post_iterator, req);
    req->data = 0;
    req->cheats_state = NULL;
    req->avatar_upload_state = NULL;
    req->pkg_upload_state = NULL;
    req->fs_upload_state = NULL;
    req->payload_upload_state = NULL;
    req->xml_upload_state = NULL;
    return MHD_YES;
  }

  if(!strcmp(method, MHD_HTTP_METHOD_GET)) {
    if(!strcmp("/fs", url)) {
      return fs_request(conn, url);
    }
    if(!strncmp("/fs/", url, 4)) {
      return fs_request(conn, url);
    }
#ifdef __SCE__
    if(!strcmp("/mdns", url)) {
      return mdns_request(conn, url);
    }
    if(!strncmp("/smb", url, 4)) {
      return smb_request(conn, url);
    }
#endif
    if(!strcmp("/launch", url)) {
      return launch_request(conn);
    }
    if(!strcmp("/cheats", url)) {
      static const char body[] =
        "<!doctype html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>CheatRunner \xe2\x80\x94 Elf Arsenal</title>"
        "<style>"
        "*{margin:0;padding:0;box-sizing:border-box}"
        "html,body{height:100%;display:flex;flex-direction:column;background:#0d0f13}"
        "#topbar{background:#0d0f13;border-bottom:1px solid rgba(110,180,255,0.2);"
        "padding:8px 14px;display:flex;align-items:center;gap:12px;flex-shrink:0;"
        "font-family:system-ui,sans-serif}"
        "#topbar a{color:#aedcff;text-decoration:none;font-size:13px;font-weight:600}"
        "#topbar a:hover{text-decoration:underline}"
        "#topbar span{color:rgba(255,255,255,0.3);font-size:13px}"
        "iframe{flex:1;border:none;width:100%}"
        "</style></head><body>"
        "<div id='topbar'>"
        "<a href='/'>&#8592; Elf Arsenal</a>"
        "<span>|</span>"
        "<span>CheatRunner</span>"
        "</div>"
        "<iframe id='cr' src='' allowfullscreen></iframe>"
        "<script>"
        "document.getElementById('cr').src='http://'+location.hostname+':9999/';"
        "</script>"
        "</body></html>";
      struct MHD_Response *r = MHD_create_response_from_buffer(
          sizeof(body) - 1, (void*)body, MHD_RESPMEM_PERSISTENT);
      if(!r) return MHD_NO;
      MHD_add_response_header(r, "Content-Type", "text/html; charset=utf-8");
      enum MHD_Result rc = websrv_queue_response(conn, MHD_HTTP_OK, r);
      MHD_destroy_response(r);
      return rc;
    }
    if(!strcmp("/api/kill", url)) {
      return kill_request(conn);
    }
    if(!strcmp("/api/power", url)) {
      return power_request(conn);
    }
    if(!strcmp("/api/fwspoof/apply", url)) {
      return fwspoof_request(conn);
    }
    if(!strcmp("/api/trophy/unlock-now", url)) {
      int rc = sys_trophy_unlock_now();
      const char *body = rc >= 0 ? "{\"ok\":true}"
                                 : "{\"ok\":false,\"error\":\"spawn failed\"}";
      struct MHD_Response *resp = MHD_create_response_from_buffer(
          strlen(body), (void *)body, MHD_RESPMEM_PERSISTENT);
      if(!resp) return MHD_NO;
      MHD_add_response_header(resp, "Content-Type", "application/json");
      enum MHD_Result ret = websrv_queue_response(conn, MHD_HTTP_OK, resp);
      MHD_destroy_response(resp);
      return ret;
    }
    if(!strcmp("/api/gameinfo", url)) {
      return gameinfo_request(conn);
    }
    if(!strcmp("/api/state", url)) {
      return state_request(conn);
    }
    if(!strncmp("/api/linux", url, 10)) {
      return linux_loader_request(conn, url, method);
    }
    if(!strncmp("/api/cheats", url, 11) || !strcmp("/api/cr/eboot", url)) {
      return cheats_request(conn, url, method, NULL, NULL, &req->cheats_state);
    }
    if(!strncmp("/api/homebrew", url, 13)) {
      return homebrew_request(conn, url);
    }
    if(!strncmp("/api/avatar", url, 11)) {
      return avatar_request(conn, url);
    }
    if(!strncmp("/api/kstuff", url, 11)) {
      return kstuff_updater_request(conn, url);
    }
    if(!strncmp("/api/smp", url, 8)) {
      return smp_updater_request(conn, url);
    }
    if(!strncmp("/api/y2jb", url, 9)) {
      return y2jb_request(conn, url);
    }
    if(!strncmp("/api/np", url, 7)) {
      return np_request(conn, url);
    }
    if(!strncmp("/api/sdk-changer", url, 16)) {
      return sdk_changer_request(conn, url);
    }
    if(!strncmp("/api/offline/", url, 13)) {
      return offline_pack_request(conn, url);
    }
    if(!strncmp("/api/db/", url, 8)) {
      return db_snap_request(conn, url);
    }
    if(!strncmp("/api/backup", url, 11)) {
      return backup_request(conn, url);
    }
    if(!strncmp("/api/garlic", url, 11)) {
      return garlic_request(conn, url);
    }
    if(!strncmp("/api/dumper/", url, 12)) {
      return dumper_request(conn, url);
    }
    if(!strncmp("/api/fs/", url, 8)) {
      return transfer_request(conn, url);
    }
    if(!strncmp("/api/fan", url, 8)) {
      return fan_request(conn, url);
    }
    if(!strncmp("/api/notifications", url, 18)) {
      return notif_inbox_request(conn, url);
    }
    if(!strncmp("/api/klogs", url, 10)) {
      return dashboards_klogs_request(conn, url);
    }
    if(!strcmp("/api/stats", url)) {
      return dashboards_stats_request(conn);
    }
    if(!strcmp("/api/sensors", url)) {
      extern enum MHD_Result sensors_request(struct MHD_Connection*);
      return sensors_request(conn);
    }
    if(!strcmp("/api/sensors/drives", url)) {
      return drive_sensors_request(conn);
    }
    if(!strncmp("/api/translate", url, 14)) {
      return translate_request(conn, url);
    }
    if(!strncmp("/api/activitydb", url, 15)) {
      return activitydb_request(conn, url);
    }
    if(!strncmp("/api/activity", url, 13)) {
      return activity_request(conn, url);
    }
    if(!strncmp("/api/releases", url, 13)) {
      extern enum MHD_Result releases_request(struct MHD_Connection*, const char*);
      return releases_request(conn, url);
    }
    if(!strncmp("/api/pkgzone/", url, 13)) {
      extern enum MHD_Result pkgzone_request(struct MHD_Connection*, const char*);
      return pkgzone_request(conn, url);
    }
    if(!strncmp("/api/payloads/", url, 14)) {
      extern enum MHD_Result payload_registry_request(struct MHD_Connection*, const char*);
      return payload_registry_request(conn, url);
    }
    if(!strncmp("/api/plugins", url, 12)) {
      extern enum MHD_Result plugin_loader_request(struct MHD_Connection*, const char*);
      return plugin_loader_request(conn, url);
    }
    if(!strncmp("/api/xmlpatches", url, 15)) {
      extern enum MHD_Result xml_patches_request(struct MHD_Connection*, const char*);
      return xml_patches_request(conn, url);
    }
    if(!strncmp("/api/ftpsrv", url, 11)) {
      char body[512];
      int len = 0;
      if(!strcmp(url, "/api/ftpsrv")) {
        const char *u = sys_ftpsrv_get_user();
        const char *p = sys_ftpsrv_get_pass();
        const char *t = sys_ftpsrv_get_type();
        int has_auth = (u && *u && strcasecmp(u, "anonymous") != 0);
        const char *d = sys_ftpsrv_get_daemon();
        len = snprintf(body, sizeof(body),
            "{\"ok\":true,\"running\":%s,\"port\":%d,"
            "\"user\":\"%s\",\"pass\":\"%s\",\"hasPassword\":%s,"
            "\"transferDefault\":\"%s\","
            "\"authMode\":\"%s\","
            "\"daemon\":\"%s\","
            "\"authConfigurable\":true,\"transferConfigurable\":true}",
            sys_ftpsrv_is_running() ? "true" : "false",
            sys_ftpsrv_get_port(),
            u ? u : "anonymous",
            p ? p : "",
            p && p[0] ? "true" : "false",
            t ? t : "auto",
            has_auth ? "userpass" : "anonymous",
            d ? d : "ftpsrv");
      } else if(!strcmp(url, "/api/ftpsrv/toggle")) {
        const char *on = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "on");
        int want = (on && (!strcmp(on, "1") || !strcasecmp(on, "true"))) ? 1 : 0;
        int rc = sys_ftpsrv_set_enabled(want);
        len = snprintf(body, sizeof(body),
            "{\"ok\":%s,\"running\":%s,\"port\":%d}",
            rc >= 0 ? "true" : "false",
            sys_ftpsrv_is_running() ? "true" : "false",
            sys_ftpsrv_get_port());
      } else if(!strcmp(url, "/api/ftpsrv/restart")) {
        int rc = sys_ftpsrv_restart();
        len = snprintf(body, sizeof(body),
            "{\"ok\":%s,\"running\":%s,\"port\":%d}",
            rc == 0 ? "true" : "false",
            sys_ftpsrv_is_running() ? "true" : "false",
            sys_ftpsrv_get_port());
      } else if(!strcmp(url, "/api/ftpsrv/port")) {
        const char *v = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "value");
        int port = v ? atoi(v) : 0;
        if(port < 1 || port > 65535) {
          const char *err = "{\"ok\":false,\"error\":\"port must be 1..65535\"}";
          struct MHD_Response *r =
              MHD_create_response_from_buffer(strlen(err), (void*)err,
                                              MHD_RESPMEM_PERSISTENT);
          MHD_add_response_header(r, "Content-Type", "application/json");
          enum MHD_Result rc = websrv_queue_response(conn, 400, r);
          MHD_destroy_response(r);
          return rc;
        }
        sys_ftpsrv_set_port(port);
        extern void config_save(void);
        config_save();
        int rc = sys_ftpsrv_restart();
        len = snprintf(body, sizeof(body),
            "{\"ok\":%s,\"running\":%s,\"port\":%d,\"restarted\":%s}",
            rc == 0 ? "true" : "false",
            sys_ftpsrv_is_running() ? "true" : "false",
            port,
            rc == 0 ? "true" : "false");
      } else if(!strcmp(url, "/api/ftpsrv/auth")) {
        const char *user = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "user");
        const char *pass = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "pass");
        sys_ftpsrv_set_user(user);  /* empty / "anonymous" -> open */
        sys_ftpsrv_set_pass(pass);
        extern void config_save(void); config_save();
        int rc = sys_ftpsrv_restart();
        const char *u = sys_ftpsrv_get_user();
        const char *p = sys_ftpsrv_get_pass();
        int has_auth = (u && *u && strcasecmp(u, "anonymous") != 0);
        int has_pass = p && p[0] != '\0';
        len = snprintf(body, sizeof(body),
            "{\"ok\":%s,\"running\":%s,\"user\":\"%s\","
            "\"pass\":\"%s\",\"hasPassword\":%s,"
            "\"authMode\":\"%s\",\"restarted\":%s}",
            rc == 0 ? "true" : "false",
            sys_ftpsrv_is_running() ? "true" : "false",
            u ? u : "anonymous",
            p ? p : "",
            has_pass ? "true" : "false",
            has_auth ? "userpass" : "anonymous",
            rc == 0 ? "true" : "false");
      } else if(!strcmp(url, "/api/ftpsrv/type")) {
        const char *v = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "value");
        sys_ftpsrv_set_type(v ? v : "auto");
        extern void config_save(void); config_save();
        int rc = sys_ftpsrv_restart();
        len = snprintf(body, sizeof(body),
            "{\"ok\":%s,\"running\":%s,\"transferDefault\":\"%s\","
            "\"restarted\":%s}",
            rc == 0 ? "true" : "false",
            sys_ftpsrv_is_running() ? "true" : "false",
            sys_ftpsrv_get_type(),
            rc == 0 ? "true" : "false");
      } else if(!strcmp(url, "/api/ftpsrv/daemon")) {
        /* Switch the FTP daemon between bundled ftpsrv and zftpd. */
        const char *v = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "value");
        sys_ftpsrv_set_daemon(v ? v : "ftpsrv");
        extern void config_save(void); config_save();
        int rc = sys_ftpsrv_restart();
        len = snprintf(body, sizeof(body),
            "{\"ok\":%s,\"running\":%s,\"daemon\":\"%s\","
            "\"restarted\":%s}",
            rc == 0 ? "true" : "false",
            sys_ftpsrv_is_running() ? "true" : "false",
            sys_ftpsrv_get_daemon(),
            rc == 0 ? "true" : "false");
      } else {
        const char *err = "{\"ok\":false,\"error\":\"no such endpoint\"}";
        struct MHD_Response *r =
            MHD_create_response_from_buffer(strlen(err), (void*)err,
                                            MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(r, "Content-Type", "application/json");
        enum MHD_Result rc = websrv_queue_response(conn, 404, r);
        MHD_destroy_response(r);
        return rc;
      }
      char *out = malloc(len + 1);
      if(!out) return MHD_NO;
      memcpy(out, body, len + 1);
      struct MHD_Response *r =
          MHD_create_response_from_buffer(len, out, MHD_RESPMEM_MUST_FREE);
      MHD_add_response_header(r, "Content-Type", "application/json");
      enum MHD_Result rc = websrv_queue_response(conn, 200, r);
      MHD_destroy_response(r);
      return rc;
    }
    if(!strncmp("/api/offact", url, 11)) {
      char body[4096];
      int len = 0;
      if(!strcmp(url, "/api/offact")) {
        len = snprintf(body, sizeof(body), "{\"ok\":true,\"slots\":[");
        int first = 1;
        for(int s = 1; s <= OFFACT_SLOT_COUNT; s++) {
          char name[OFFACT_NAME_MAX] = {0};
          char type[OFFACT_TYPE_MAX] = {0};
          uint64_t id = 0;
          int flags = 0;
          if(offact_get_name(s, name) || !name[0]) continue;
          offact_get_id(s, &id);
          offact_get_type(s, type);
          offact_get_flags(s, &flags);
          char esc[OFFACT_NAME_MAX * 2];
          int eo = 0;
          for(int i = 0; name[i] && eo < (int)sizeof(esc) - 2; i++) {
            if(name[i] == '"' || name[i] == '\\') esc[eo++] = '\\';
            esc[eo++] = name[i];
          }
          esc[eo] = 0;
          int activated = (id != 0 && flags == OFFACT_DEFAULT_FLAGS);
          int n = snprintf(body + len, sizeof(body) - len,
              "%s{\"slot\":%d,\"name\":\"%s\",\"type\":\"%s\","
              "\"flags\":%d,\"id\":\"0x%016lx\",\"activated\":%s}",
              first ? "" : ",", s, esc, type, flags,
              (unsigned long)id, activated ? "true" : "false");
          if(n <= 0 || n >= (int)(sizeof(body) - len)) break;
          len += n;
          first = 0;
        }
        len += snprintf(body + len, sizeof(body) - len, "]}");
      } else if(!strcmp(url, "/api/offact/activate")) {
        const char *slot_s = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "slot");
        const char *id_s = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "id");
        int slot = slot_s ? atoi(slot_s) : 0;
        uint64_t id = 0;
        if(id_s && *id_s) {
          /* Accept "0xHEX" or plain hex/decimal. */
          if(id_s[0] == '0' && (id_s[1] == 'x' || id_s[1] == 'X'))
            id = strtoull(id_s + 2, NULL, 16);
          else
            id = strtoull(id_s, NULL, 0);
        }
        int rc = offact_activate(slot, id);
        uint64_t actual = 0;
        offact_get_id(slot, &actual);
        len = snprintf(body, sizeof(body),
            "{\"ok\":%s,\"slot\":%d,\"id\":\"0x%016lx\"}",
            rc == 0 ? "true" : "false", slot, (unsigned long)actual);
      } else if(!strcmp(url, "/api/offact/id")) {
        /* Set just the account id without touching type/flags. */
        const char *slot_s = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "slot");
        const char *id_s = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "id");
        int slot = slot_s ? atoi(slot_s) : 0;
        uint64_t id = 0;
        int parsed = 0;
        if(id_s && *id_s) {
          if(id_s[0] == '0' && (id_s[1] == 'x' || id_s[1] == 'X'))
            id = strtoull(id_s + 2, NULL, 16);
          else
            id = strtoull(id_s, NULL, 0);
          parsed = 1;
        }
        int rc = parsed ? offact_set_id(slot, id) : -1;
        len = snprintf(body, sizeof(body),
            "{\"ok\":%s,\"slot\":%d,\"id\":\"0x%016lx\"}",
            rc == 0 ? "true" : "false", slot, (unsigned long)id);
      } else if(!strcmp(url, "/api/offact/type")) {
        /* Set just the type string (e.g. "np", "psn", or any short label). */
        const char *slot_s = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "slot");
        const char *type = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "type");
        int slot = slot_s ? atoi(slot_s) : 0;
        int rc = (type && *type) ? offact_set_type(slot, type) : -1;
        len = snprintf(body, sizeof(body),
            "{\"ok\":%s,\"slot\":%d,\"type\":\"%s\"}",
            rc == 0 ? "true" : "false", slot, type ? type : "");
      } else if(!strcmp(url, "/api/offact/flags")) {
        /* Set just the flags integer (decimal or 0xHEX). */
        const char *slot_s = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "slot");
        const char *flags_s = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "flags");
        int slot = slot_s ? atoi(slot_s) : 0;
        int flags = 0, parsed = 0;
        if(flags_s && *flags_s) {
          flags = (int)strtol(flags_s, NULL, 0);
          parsed = 1;
        }
        int rc = parsed ? offact_set_flags(slot, flags) : -1;
        len = snprintf(body, sizeof(body),
            "{\"ok\":%s,\"slot\":%d,\"flags\":%d}",
            rc == 0 ? "true" : "false", slot, flags);
      } else if(!strcmp(url, "/api/offact/clear")) {
        const char *slot_s = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "slot");
        int slot = slot_s ? atoi(slot_s) : 0;
        int rc = offact_clear(slot);
        len = snprintf(body, sizeof(body),
            "{\"ok\":%s,\"slot\":%d}",
            rc == 0 ? "true" : "false", slot);
      } else if(!strcmp(url, "/api/offact/rename")) {
        const char *slot_s = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "slot");
        const char *name = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "name");
        int slot = slot_s ? atoi(slot_s) : 0;
        int rc = (name && *name) ? offact_set_name(slot, name) : -1;
        len = snprintf(body, sizeof(body),
            "{\"ok\":%s,\"slot\":%d,\"name\":\"%s\"}",
            rc == 0 ? "true" : "false", slot, name ? name : "");
      } else {
        const char *err = "{\"ok\":false,\"error\":\"no such endpoint\"}";
        struct MHD_Response *r =
            MHD_create_response_from_buffer(strlen(err), (void*)err,
                                            MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(r, "Content-Type", "application/json");
        enum MHD_Result rc = websrv_queue_response(conn, 404, r);
        MHD_destroy_response(r);
        return rc;
      }
      char *out = malloc(len + 1);
      if(!out) return MHD_NO;
      memcpy(out, body, len + 1);
      struct MHD_Response *r =
          MHD_create_response_from_buffer(len, out, MHD_RESPMEM_MUST_FREE);
      MHD_add_response_header(r, "Content-Type", "application/json");
      enum MHD_Result rc = websrv_queue_response(conn, 200, r);
      MHD_destroy_response(r);
      return rc;
    }
    if(!strncmp("/appdb", url, 6) && (url[6] == 0 || url[6] == '/')) {
      return appdb_request(conn, url);
    }
    if(!strncmp("/api/tmdb/", url, 10)) {
      return tmdb_request(conn, url);
    }
    if(!strcmp("/hbldr", url)) {
      return hbldr_request(conn);
    }
    if(!strcmp("/elfldr", url)) {
      return elfldr_request(conn, 0);
    }
    if(!strcmp("/version", url)) {
      return version_request(conn);
    }
    if(!strcmp("/", url) || !url[0] ||
       !strcmp("/launcher.html", url) || !strcmp("/index.html", url)) {
      struct stat st_redir;
      if(stat("/data/elf-arsenal/.first_boot_redirect_pending", &st_redir)
         == 0) {
        unlink("/data/elf-arsenal/.first_boot_redirect_pending");
        struct MHD_Response *r = MHD_create_response_from_buffer(
            0, "", MHD_RESPMEM_PERSISTENT);
        if(r) {
          MHD_add_response_header(r, "Location",
              "/launcher.html#kstuff-update-card");
          MHD_add_response_header(r, "Cache-Control", "no-store");
          enum MHD_Result rc = websrv_queue_response(conn,
              MHD_HTTP_FOUND, r);
          MHD_destroy_response(r);
          return rc;
        }
      }
      return asset_request(conn, "/launcher.html");
    }
    if(!strcmp("/homebrew", url) ||
       !strncmp("/homebrew/", url, 10)) {
      return asset_request(conn, "/index.html");
    }
    if(!strcmp("/files", url)) {
      return asset_request(conn, "/files.html");
    }
    if(!strcmp("/klog", url)) {
      return asset_request(conn, "/klog.html");
    }
    if(!strcmp("/stats", url)) {
      return asset_request(conn, "/stats.html");
    }
    if(!strcmp("/sensors", url)) {
      return asset_request(conn, "/sensors.html");
    }
    if(!strcmp("/pkgzone", url)) {
      return asset_request(conn, "/pkgzone.html");
    }
    if(!strcmp("/cheats", url)) {
      return asset_request(conn, "/cheats.html");
    }
    if(!strcmp("/help", url)) {
      return asset_request(conn, "/help.html");
    }
    if(!strcmp("/savemgr", url)) {
      return asset_request(conn, "/savemgr.html");
    }
    if(!strcmp("/fpkg-guard", url)) {
      return asset_request(conn, "/fpkg-guard.html");
    }
    if(!strcmp("/tskmgr", url)) {
      return asset_request(conn, "/tskmgr.html");
    }
    if(!strcmp("/api/tskmgr/procs", url)) {
      char *json = sys_proc_list_json();
      if(!json) json = strdup("[]");
      struct MHD_Response *r = MHD_create_response_from_buffer(
        strlen(json), json, MHD_RESPMEM_MUST_FREE);
      MHD_add_response_header(r, "Content-Type", "application/json");
      enum MHD_Result rc = websrv_queue_response(conn, MHD_HTTP_OK, r);
      MHD_destroy_response(r);
      return rc;
    }
    if(!strcmp("/api/tskmgr/kill", url)) {
      const char *pid_s = MHD_lookup_connection_value(
        conn, MHD_GET_ARGUMENT_KIND, "pid");
      char body[64];
      unsigned int status = MHD_HTTP_OK;
      if(!pid_s) {
        snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"missing pid\"}");
        status = MHD_HTTP_BAD_REQUEST;
      } else {
        pid_t target = (pid_t)atoi(pid_s);
        if(target <= 1 || target == getpid()) {
          snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"forbidden\"}");
        } else {
          /* Aggressive kill: send SIGKILL, poll every 50 ms until the PID
             disappears (ESRCH), retry up to 10 times (~500 ms max). */
          int dead = 0;
          for(int i = 0; i < 10 && !dead; i++) {
            int r = kill(target, SIGKILL);
            if(r != 0 && errno == ESRCH) { dead = 1; break; }
            struct timespec ts = {0, 50 * 1000 * 1000L};
            nanosleep(&ts, NULL);
            if(kill(target, 0) != 0 && errno == ESRCH) dead = 1;
          }
          if(dead)
            snprintf(body, sizeof(body), "{\"ok\":true}");
          else
            snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"process did not die\"}");
        }
      }
      struct MHD_Response *r = MHD_create_response_from_buffer(
        strlen(body), body, MHD_RESPMEM_PERSISTENT);
      MHD_add_response_header(r, "Content-Type", "application/json");
      enum MHD_Result rc = websrv_queue_response(conn, status, r);
      MHD_destroy_response(r);
      return rc;
    }
    return asset_request(conn, url);
  }

  if(!strcmp(method, MHD_HTTP_METHOD_POST)) {
    if(!strcmp("/api/avatar/upload", url)) {
      return avatar_upload_request(conn, upload_data, upload_data_size,
                                   &req->avatar_upload_state);
    }
    if(!strcmp("/api/homebrew/install-pkg-upload", url)) {
      return pkg_upload_request(conn, upload_data, upload_data_size,
                                &req->pkg_upload_state);
    }
    if(!strcmp("/api/fs/upload", url)) {
      return fs_upload_request(conn, upload_data, upload_data_size,
                               &req->fs_upload_state);
    }
    if(!strcmp("/api/payloads/upload", url)) {
      extern enum MHD_Result payload_upload_request(struct MHD_Connection*,
                                                    const char*, size_t*, void**);
      return payload_upload_request(conn, upload_data, upload_data_size,
                                    &req->payload_upload_state);
    }
    if(!strcmp("/api/xmlpatches/upload", url)) {
      extern enum MHD_Result xml_patches_upload_request(struct MHD_Connection*,
                                                        const char*, size_t*, void**);
      return xml_patches_upload_request(conn, upload_data, upload_data_size,
                                        &req->xml_upload_state);
    }
    if(*upload_data_size) {
      ret = MHD_post_process(req->pp, upload_data, *upload_data_size);
      *upload_data_size = 0;
      return ret;
    }
    if(!strcmp("/elfldr", url)) {
      return elfldr_request(conn, req->data);
    }
    /* Notifications inbox actions ride POST so they're side-effecting
       (mark all read / clear). The handler validates the URL itself. */
    if(!strncmp("/api/notifications", url, 18)) {
      return notif_inbox_request(conn, url);
    }
    if(!strcmp("/api/fan/curve/set", url)) {
      return fan_request(conn, url);
    }
    if(!strncmp("/api/klogs", url, 10)) {
      return dashboards_klogs_request(conn, url);
    }
    if(!strncmp("/api/dumper/", url, 12)) {
      return dumper_request(conn, url);
    }
    if(!strncmp("/api/sdk-changer", url, 16)) {
      return sdk_changer_request(conn, url);
    }
    if(!strncmp("/api/offline/", url, 13)) {
      return offline_pack_request(conn, url);
    }
    /* Cheat repo download + status are side-effecting; the UI uses POST.
       Same routing as the GET block above so cheats_request sees both. */
    if(!strncmp("/api/cheats", url, 11)) {
      return cheats_request(conn, url, method, upload_data,
                            upload_data_size, &req->cheats_state);
    }
    /* i18n.js sends all translate calls as POST (batch, cache, clear). */
    if(!strncmp("/api/translate", url, 14)) {
      return translate_request(conn, url);
    }
  }

  return MHD_NO;
}



static void
websrv_on_completed(void *cls, struct MHD_Connection *connection,
                    void **con_cls, enum MHD_RequestTerminationCode toe) {
  post_request_t *req = *con_cls;
  post_data_t *data;

  if(!req) {
    return;
  }

  while((data=req->data)) {
    req->data = data->next;
    free(data->key);
    free(data->val);
    free(data);
  }

  if(req->cheats_state) {
    struct { char *buf; size_t cap; size_t len; } *u = req->cheats_state;
    free(u->buf);
    free(u);
  }

  if(req->avatar_upload_state) {
    avatar_upload_free(req->avatar_upload_state);
  }
  if(req->pkg_upload_state) {
    pkg_upload_free(req->pkg_upload_state);
  }
  if(req->fs_upload_state) {
    fs_upload_free(req->fs_upload_state);
  }
  if(req->payload_upload_state) {
    extern void payload_upload_free(void*);
    payload_upload_free(req->payload_upload_state);
  }

  MHD_destroy_post_processor(req->pp);
  free(req);
}


static int                  g_websrv_srvfd  = -1;
static struct MHD_Daemon   *g_websrv_httpd  = NULL;
static pthread_mutex_t      g_websrv_lock   = PTHREAD_MUTEX_INITIALIZER;

void
websrv_pause(void) {
  pthread_mutex_lock(&g_websrv_lock);
  int fd = g_websrv_srvfd;
  struct MHD_Daemon *h = g_websrv_httpd;
  g_websrv_srvfd = -1;
  g_websrv_httpd = NULL;
  pthread_mutex_unlock(&g_websrv_lock);

  if (fd >= 0) shutdown(fd, SHUT_RDWR);
  (void)h;   /* httpd is destroyed by the websrv_listen tail */
  fprintf(stderr, "websrv_pause: srvfd shut down, websrv exiting\n");
}


int
websrv_listen(unsigned short port) {
  struct sockaddr_in server_addr;
  struct sockaddr_in client_addr;
  struct MHD_Daemon *httpd;
  socklen_t addr_len;
  int connfd;
  int srvfd;

  signal(SIGPIPE, SIG_IGN);

  if((srvfd=socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket");
    return -1;
  }

  if(setsockopt(srvfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
    perror("setsockopt");
    close(srvfd);
    return -1;
  }

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(port);

  if(bind(srvfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
    perror("bind");
    close(srvfd);
    return -1;
  }

  if(listen(srvfd, 5) != 0) {
    perror("listen");
    close(srvfd);
    return -1;
  }

  if(!(httpd=MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION | MHD_USE_ITC |
			      MHD_USE_NO_LISTEN_SOCKET | MHD_USE_DEBUG |
			      MHD_USE_INTERNAL_POLLING_THREAD,
			      0, NULL, NULL, &websrv_on_request, NULL,
                              MHD_OPTION_NOTIFY_COMPLETED, &websrv_on_completed,
                              NULL, MHD_OPTION_END))) {
    perror("MHD_start_daemon");
    close(srvfd);
    return -1;
  }

  /* Publish handles for websrv_pause(). */
  pthread_mutex_lock(&g_websrv_lock);
  g_websrv_srvfd = srvfd;
  g_websrv_httpd = httpd;
  pthread_mutex_unlock(&g_websrv_lock);

  while(1) {
    addr_len = sizeof(client_addr);
    if((connfd=accept(srvfd, (struct sockaddr*)&client_addr, &addr_len)) < 0) {
      perror("accept");
      break;
    }

    if(MHD_add_connection(httpd, connfd, (struct sockaddr*)&client_addr,
			  addr_len) != MHD_YES) {
      perror("MHD_add_connection");
      break;
    }
  }

  /* Clear the global handles BEFORE tearing down so concurrent
     websrv_pause() calls don't try to shutdown a dying fd. */
  pthread_mutex_lock(&g_websrv_lock);
  g_websrv_srvfd = -1;
  g_websrv_httpd = NULL;
  pthread_mutex_unlock(&g_websrv_lock);

  MHD_stop_daemon(httpd);

  return close(srvfd);
}


