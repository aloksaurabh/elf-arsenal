
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <microhttpd.h>

#include "notif_inbox.h"
#include "third_party/cJSON.h"
#include "websrv.h"

#define INBOX_CAP        64
#define MSG_MAX          256
#define NOTIF_DIR        "/data/elf-arsenal"
#define NOTIF_PATH       "/data/elf-arsenal/notifications.json"
#define NOTIF_TMP_PATH   "/data/elf-arsenal/notifications.json.tmp"

struct entry {
  uint64_t  seq;       /* monotonic; lets the UI know what's new */
  time_t    ts;
  uint8_t   read;
  char      msg[MSG_MAX];
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct entry    g_buf[INBOX_CAP];
static int             g_count = 0;       /* total entries (capped at CAP) */
static int             g_head  = 0;       /* next write slot */
static uint64_t        g_seq   = 0;
static int             g_unread = 0;


static void
persist_locked(void) {
  mkdir("/data", 0755);
  mkdir(NOTIF_DIR, 0755);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "seq", (double)g_seq);
  cJSON *arr = cJSON_AddArrayToObject(root, "items");

  int start = (g_head - g_count + INBOX_CAP) % INBOX_CAP;
  for (int i = 0; i < g_count; i++) {
    struct entry *e = &g_buf[(start + i) % INBOX_CAP];
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "seq",  (double)e->seq);
    cJSON_AddNumberToObject(o, "ts",   (double)e->ts);
    cJSON_AddBoolToObject  (o, "read", e->read ? 1 : 0);
    cJSON_AddStringToObject(o, "msg",  e->msg);
    cJSON_AddItemToArray(arr, o);
  }

  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!body) return;

  FILE *f = fopen(NOTIF_TMP_PATH, "w");
  if (!f) { free(body); return; }
  size_t n = strlen(body);
  fwrite(body, 1, n, f);
  fclose(f);
  free(body);

  if (rename(NOTIF_TMP_PATH, NOTIF_PATH) != 0) {
    unlink(NOTIF_TMP_PATH);
  }
}


void
notif_inbox_init(void) {
  FILE *f = fopen(NOTIF_PATH, "r");
  if (!f) return;
  fseek(f, 0, SEEK_END);
  long fsz = ftell(f);
  if (fsz <= 0 || fsz > 4 * 1024 * 1024) { fclose(f); return; }
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)fsz + 1);
  if (!buf) { fclose(f); return; }
  size_t got = fread(buf, 1, (size_t)fsz, f);
  fclose(f);
  buf[got] = 0;

  cJSON *root = cJSON_Parse(buf);
  free(buf);
  if (!root) return;

  pthread_mutex_lock(&g_lock);
  cJSON *seq_j = cJSON_GetObjectItem(root, "seq");
  if (cJSON_IsNumber(seq_j)) g_seq = (uint64_t)seq_j->valuedouble;

  cJSON *arr = cJSON_GetObjectItem(root, "items");
  if (cJSON_IsArray(arr)) {
    cJSON *o;
    cJSON_ArrayForEach(o, arr) {
      if (g_count >= INBOX_CAP) break;
      cJSON *seq_e  = cJSON_GetObjectItem(o, "seq");
      cJSON *ts_e   = cJSON_GetObjectItem(o, "ts");
      cJSON *read_e = cJSON_GetObjectItem(o, "read");
      cJSON *msg_e  = cJSON_GetObjectItem(o, "msg");
      if (!cJSON_IsString(msg_e) || !msg_e->valuestring) continue;

      struct entry *e = &g_buf[g_head];
      e->seq  = cJSON_IsNumber(seq_e) ? (uint64_t)seq_e->valuedouble : ++g_seq;
      e->ts   = cJSON_IsNumber(ts_e)  ? (time_t)ts_e->valuedouble    : time(NULL);
      e->read = cJSON_IsTrue(read_e)  ? 1 : 0;
      size_t n = strlen(msg_e->valuestring);
      if (n >= MSG_MAX) n = MSG_MAX - 1;
      memcpy(e->msg, msg_e->valuestring, n);
      e->msg[n] = 0;

      g_head = (g_head + 1) % INBOX_CAP;
      g_count++;
      if (!e->read) g_unread++;
      if (e->seq > g_seq) g_seq = e->seq;
    }
  }
  pthread_mutex_unlock(&g_lock);
  cJSON_Delete(root);
}


void
notif_inbox_push(const char *msg) {
  if (!msg) return;
  pthread_mutex_lock(&g_lock);
  struct entry *e = &g_buf[g_head];
  /* If we're overwriting an unread entry, decrement the counter. */
  if (g_count == INBOX_CAP && !e->read) {
    if (g_unread > 0) g_unread--;
  }
  g_seq++;
  e->seq  = g_seq;
  e->ts   = time(NULL);
  e->read = 0;
  size_t n = strlen(msg);
  if (n >= MSG_MAX) n = MSG_MAX - 1;
  memcpy(e->msg, msg, n);
  e->msg[n] = 0;

  g_head = (g_head + 1) % INBOX_CAP;
  if (g_count < INBOX_CAP) g_count++;
  g_unread++;
  persist_locked();
  pthread_mutex_unlock(&g_lock);
}


/* Build a JSON snapshot. Items in chronological order (oldest first). */
static cJSON *
build_snapshot(void) {
  cJSON *r = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(r, "items");

  pthread_mutex_lock(&g_lock);
  /* The oldest entry sits at (g_head - g_count) mod CAP. */
  int start = (g_head - g_count + INBOX_CAP) % INBOX_CAP;
  for (int i = 0; i < g_count; i++) {
    struct entry *e = &g_buf[(start + i) % INBOX_CAP];
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "seq",  (double)e->seq);
    cJSON_AddNumberToObject(o, "ts",   (double)e->ts);
    cJSON_AddBoolToObject  (o, "read", e->read ? 1 : 0);
    cJSON_AddStringToObject(o, "msg",  e->msg);
    cJSON_AddItemToArray(arr, o);
  }
  cJSON_AddNumberToObject(r, "unread", (double)g_unread);
  cJSON_AddNumberToObject(r, "total",  (double)g_count);
  pthread_mutex_unlock(&g_lock);
  return r;
}


static enum MHD_Result
serve_json_obj(struct MHD_Connection *c, cJSON *r, int code) {
  char *body = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  struct MHD_Response *resp = MHD_create_response_from_buffer(
      strlen(body), body, MHD_RESPMEM_MUST_FREE);
  MHD_add_response_header(resp, "Content-Type", "application/json");
  MHD_add_response_header(resp, "Cache-Control", "no-store");
  enum MHD_Result rc = MHD_queue_response(c, code, resp);
  MHD_destroy_response(resp);
  return rc;
}


enum MHD_Result
notif_inbox_request(struct MHD_Connection *c, const char *url) {
  if (!strcmp(url, "/api/notifications")) {
    return serve_json_obj(c, build_snapshot(), MHD_HTTP_OK);
  }
  if (!strcmp(url, "/api/notifications/read")) {
    pthread_mutex_lock(&g_lock);
    int start = (g_head - g_count + INBOX_CAP) % INBOX_CAP;
    for (int i = 0; i < g_count; i++) g_buf[(start + i) % INBOX_CAP].read = 1;
    g_unread = 0;
    persist_locked();
    pthread_mutex_unlock(&g_lock);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 1);
    return serve_json_obj(c, r, MHD_HTTP_OK);
  }
  if (!strcmp(url, "/api/notifications/clear")) {
    pthread_mutex_lock(&g_lock);
    g_count = 0;
    g_head  = 0;
    g_unread = 0;
    /* Wipe the persisted file too — only an explicit Clear all from the
       UI ever empties the inbox, so the on-disk snapshot has to follow. */
    unlink(NOTIF_PATH);
    pthread_mutex_unlock(&g_lock);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 1);
    return serve_json_obj(c, r, MHD_HTTP_OK);
  }
  return MHD_NO;
}
