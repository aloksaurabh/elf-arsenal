
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <microhttpd.h>

#include "activity.h"
#include "third_party/cJSON.h"
#include "websrv.h"


#define ACTIVITY_DIR     "/data/elf-arsenal"
#define ACTIVITY_FILE    "/data/elf-arsenal/activity.json"
#define ACTIVITY_TMP     "/data/elf-arsenal/activity.json.tmp"
#define MAX_TITLES       4096
#define LAUNCH_DEBOUNCE  5


typedef struct {
  char     title_id[16];      /* "CUSA09100" / "PPSA01411" — 9 chars + NUL */
  uint64_t launches;
  uint64_t total_seconds;
  int64_t  last_launch_ts;
  int64_t  last_seen_ts;
  int64_t  session_started_ts;  /* 0 = not in session */
} entry_t;

static entry_t          g_entries[MAX_TITLES];
static int              g_count = 0;
static pthread_mutex_t  g_lock  = PTHREAD_MUTEX_INITIALIZER;
static int              g_inited = 0;


static entry_t*
find(const char *id) {
  for(int i = 0; i < g_count; i++) {
    if(!strcmp(g_entries[i].title_id, id)) return &g_entries[i];
  }
  return NULL;
}


static entry_t*
find_or_create(const char *id) {
  entry_t *e = find(id);
  if(e) return e;
  if(g_count >= MAX_TITLES) return NULL;
  e = &g_entries[g_count++];
  memset(e, 0, sizeof(*e));
  strncpy(e->title_id, id, sizeof(e->title_id) - 1);
  return e;
}


static void
serialize_locked(void) {
  cJSON *root = cJSON_CreateObject();
  for(int i = 0; i < g_count; i++) {
    const entry_t *e = &g_entries[i];
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "launches",         (double)e->launches);
    cJSON_AddNumberToObject(o, "totalSeconds",     (double)e->total_seconds);
    cJSON_AddNumberToObject(o, "lastLaunchTs",     (double)e->last_launch_ts);
    cJSON_AddNumberToObject(o, "lastSeenTs",       (double)e->last_seen_ts);
    cJSON_AddNumberToObject(o, "sessionStartedTs", (double)e->session_started_ts);
    cJSON_AddItemToObject(root, e->title_id, o);
  }
  char *txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if(!txt) return;

  mkdir(ACTIVITY_DIR, 0755);
  FILE *f = fopen(ACTIVITY_TMP, "w");
  if(!f) { free(txt); return; }
  fputs(txt, f);
  fclose(f);
  rename(ACTIVITY_TMP, ACTIVITY_FILE);
  free(txt);
}


static int64_t
now_ts(void) {
  return (int64_t)time(NULL);
}


void
activity_init(void) {
  pthread_mutex_lock(&g_lock);
  if(g_inited) { pthread_mutex_unlock(&g_lock); return; }
  g_inited = 1;
  g_count = 0;

  FILE *f = fopen(ACTIVITY_FILE, "r");
  if(!f) { pthread_mutex_unlock(&g_lock); return; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if(sz <= 0 || sz > 1024 * 1024) { fclose(f); pthread_mutex_unlock(&g_lock); return; }
  char *buf = malloc((size_t)sz + 1);
  if(!buf) { fclose(f); pthread_mutex_unlock(&g_lock); return; }
  size_t n = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[n] = '\0';

  cJSON *root = cJSON_Parse(buf);
  free(buf);
  if(!root) { pthread_mutex_unlock(&g_lock); return; }

  cJSON *o;
  cJSON_ArrayForEach(o, root) {
    if(g_count >= MAX_TITLES) break;
    const char *id = o->string;
    if(!id || strlen(id) >= sizeof(g_entries[0].title_id)) continue;
    entry_t *e = &g_entries[g_count++];
    memset(e, 0, sizeof(*e));
    strncpy(e->title_id, id, sizeof(e->title_id) - 1);
    cJSON *v;
    if((v = cJSON_GetObjectItem(o, "launches"))         && cJSON_IsNumber(v))
      e->launches      = (uint64_t)v->valuedouble;
    if((v = cJSON_GetObjectItem(o, "totalSeconds"))     && cJSON_IsNumber(v))
      e->total_seconds = (uint64_t)v->valuedouble;
    if((v = cJSON_GetObjectItem(o, "lastLaunchTs"))     && cJSON_IsNumber(v))
      e->last_launch_ts = (int64_t)v->valuedouble;
    if((v = cJSON_GetObjectItem(o, "lastSeenTs"))       && cJSON_IsNumber(v))
      e->last_seen_ts   = (int64_t)v->valuedouble;
    if((v = cJSON_GetObjectItem(o, "sessionStartedTs")) && cJSON_IsNumber(v))
      e->session_started_ts = (int64_t)v->valuedouble;

    if(e->session_started_ts > 0 &&
       e->last_seen_ts > e->session_started_ts) {
      e->total_seconds += (uint64_t)(e->last_seen_ts - e->session_started_ts);
      e->session_started_ts = 0;
    } else {
      e->session_started_ts = 0;
    }
  }
  cJSON_Delete(root);

  /* If we recovered any open sessions, write the cleaned state back. */
  serialize_locked();
  pthread_mutex_unlock(&g_lock);
}


void
activity_record_launch(const char *title_id) {
  if(!title_id || !*title_id) return;
  pthread_mutex_lock(&g_lock);
  entry_t *e = find_or_create(title_id);
  if(!e) { pthread_mutex_unlock(&g_lock); return; }
  int64_t now = now_ts();

  /* Debounce repeated launch events within 5 s. */
  if(e->session_started_ts > 0 &&
     (now - e->session_started_ts) < LAUNCH_DEBOUNCE) {
    e->last_seen_ts = now;
    pthread_mutex_unlock(&g_lock);
    return;
  }

  /* If a session was already open (no exit was seen — title crashed
     or PS5 rebooted), close it first using last_seen_ts. */
  if(e->session_started_ts > 0 && e->last_seen_ts > e->session_started_ts) {
    e->total_seconds += (uint64_t)(e->last_seen_ts - e->session_started_ts);
  }

  e->launches++;
  e->last_launch_ts     = now;
  e->last_seen_ts       = now;
  e->session_started_ts = now;
  serialize_locked();
  pthread_mutex_unlock(&g_lock);
}


void
activity_record_exit(const char *title_id) {
  if(!title_id || !*title_id) return;
  pthread_mutex_lock(&g_lock);
  entry_t *e = find(title_id);
  if(!e || e->session_started_ts == 0) {
    pthread_mutex_unlock(&g_lock);
    return;
  }
  int64_t now = now_ts();
  uint64_t delta = (now > e->session_started_ts)
                     ? (uint64_t)(now - e->session_started_ts) : 0;
  e->total_seconds      += delta;
  e->last_seen_ts        = now;
  e->session_started_ts  = 0;
  serialize_locked();
  pthread_mutex_unlock(&g_lock);
}



static enum MHD_Result
serve_buffer(struct MHD_Connection *conn, unsigned int status,
             const char *mime, void *data, size_t size, int free_after) {
  enum MHD_Result ret = MHD_NO;
  struct MHD_Response *resp;
  enum MHD_ResponseMemoryMode mode = free_after ? MHD_RESPMEM_MUST_FREE
                                                : MHD_RESPMEM_PERSISTENT;
  if((resp = MHD_create_response_from_buffer(size, data, mode))) {
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
serve_json(struct MHD_Connection *conn, unsigned int status, cJSON *o) {
  char *txt = cJSON_PrintUnformatted(o);
  if(!txt) return serve_buffer(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                               "application/json",
                               "{\"error\":\"alloc\"}", 17, 0);
  return serve_buffer(conn, status, "application/json", txt, strlen(txt), 1);
}


static cJSON*
entry_to_json_locked(const entry_t *e, int64_t now) {
  cJSON *o = cJSON_CreateObject();
  uint64_t total = e->total_seconds;
  if(e->session_started_ts > 0 && now > e->session_started_ts) {
    total += (uint64_t)(now - e->session_started_ts);
  }
  cJSON_AddNumberToObject(o, "launches",      (double)e->launches);
  cJSON_AddNumberToObject(o, "totalSeconds",  (double)total);
  cJSON_AddNumberToObject(o, "lastLaunchTs",  (double)e->last_launch_ts);
  cJSON_AddNumberToObject(o, "lastSeenTs",    (double)e->last_seen_ts);
  cJSON_AddBoolToObject  (o, "active",        e->session_started_ts > 0);
  return o;
}


static enum MHD_Result
list_request(struct MHD_Connection *conn) {
  cJSON *r = cJSON_CreateObject();
  pthread_mutex_lock(&g_lock);
  int64_t now = now_ts();
  cJSON_AddNumberToObject(r, "__nowTs", (double)now);
  for(int i = 0; i < g_count; i++) {
    cJSON_AddItemToObject(r, g_entries[i].title_id,
                          entry_to_json_locked(&g_entries[i], now));
  }
  pthread_mutex_unlock(&g_lock);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


static enum MHD_Result
title_request(struct MHD_Connection *conn) {
  const char *id = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                               "id");
  cJSON *r = cJSON_CreateObject();
  if(!id || !*id) {
    cJSON_AddBoolToObject(r, "ok", 0);
    cJSON_AddStringToObject(r, "error", "missing ?id=…");
    enum MHD_Result ret = serve_json(conn, MHD_HTTP_BAD_REQUEST, r);
    cJSON_Delete(r);
    return ret;
  }
  pthread_mutex_lock(&g_lock);
  entry_t *e = find(id);
  if(!e) {
    pthread_mutex_unlock(&g_lock);
    cJSON_AddBoolToObject(r, "ok", 0);
    cJSON_AddStringToObject(r, "error", "no activity for that title yet");
    enum MHD_Result ret = serve_json(conn, MHD_HTTP_NOT_FOUND, r);
    cJSON_Delete(r);
    return ret;
  }
  int64_t now = now_ts();
  cJSON_AddBoolToObject(r, "ok", 1);
  cJSON_AddItemToObject(r, "stats", entry_to_json_locked(e, now));
  pthread_mutex_unlock(&g_lock);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


static enum MHD_Result
reset_request(struct MHD_Connection *conn) {
  const char *id = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                               "id");
  cJSON *r = cJSON_CreateObject();
  pthread_mutex_lock(&g_lock);
  if(id && *id) {
    int kept = 0;
    for(int i = 0; i < g_count; i++) {
      if(strcmp(g_entries[i].title_id, id) != 0) {
        if(kept != i) g_entries[kept] = g_entries[i];
        kept++;
      }
    }
    g_count = kept;
    cJSON_AddStringToObject(r, "removed", id);
  } else {
    g_count = 0;
    cJSON_AddBoolToObject(r, "all", 1);
  }
  serialize_locked();
  pthread_mutex_unlock(&g_lock);
  cJSON_AddBoolToObject(r, "ok", 1);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


enum MHD_Result
activity_request(struct MHD_Connection *conn, const char *url) {
  if(!strcmp(url, "/api/activity"))         return list_request(conn);
  if(!strcmp(url, "/api/activity/title"))   return title_request(conn);
  if(!strcmp(url, "/api/activity/reset"))   return reset_request(conn);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", 0);
  cJSON_AddStringToObject(r, "error", "no such endpoint");
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_NOT_FOUND, r);
  cJSON_Delete(r);
  return ret;
}
