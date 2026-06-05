
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <microhttpd.h>

#include "dashboards.h"
#include "third_party/cJSON.h"
#include "websrv.h"



#define KLOG_LINE_MAX  512
#define KLOG_RING_CAP  1024

struct klog_entry {
  uint64_t seq;
  time_t   ts;
  char     line[KLOG_LINE_MAX];
};

static pthread_mutex_t  g_klog_lock = PTHREAD_MUTEX_INITIALIZER;
static struct klog_entry g_klog[KLOG_RING_CAP];
static int               g_klog_count = 0;
static int               g_klog_head  = 0;
static uint64_t          g_klog_seq   = 0;
static int               g_klog_paused = 0;


void
dashboards_klog_push(const char *line) {
  if (!line) return;
  pthread_mutex_lock(&g_klog_lock);
  if (g_klog_paused) {
    pthread_mutex_unlock(&g_klog_lock);
    return;
  }
  struct klog_entry *e = &g_klog[g_klog_head];
  g_klog_seq++;
  e->seq = g_klog_seq;
  e->ts  = time(NULL);
  size_t n = strlen(line);
  if (n >= KLOG_LINE_MAX) n = KLOG_LINE_MAX - 1;
  /* strip trailing \r\n for cleaner display */
  while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) n--;
  memcpy(e->line, line, n);
  e->line[n] = 0;

  g_klog_head = (g_klog_head + 1) % KLOG_RING_CAP;
  if (g_klog_count < KLOG_RING_CAP) g_klog_count++;
  pthread_mutex_unlock(&g_klog_lock);
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


static enum MHD_Result
klogs_get(struct MHD_Connection *c) {
  /* Optional ?since=<seq> — only return entries newer than seq. */
  const char *qs_since = MHD_lookup_connection_value(
      c, MHD_GET_ARGUMENT_KIND, "since");
  uint64_t since = qs_since ? (uint64_t)strtoull(qs_since, NULL, 10) : 0;

  cJSON *r   = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(r, "lines");

  pthread_mutex_lock(&g_klog_lock);
  int start = (g_klog_head - g_klog_count + KLOG_RING_CAP) % KLOG_RING_CAP;
  for (int i = 0; i < g_klog_count; i++) {
    struct klog_entry *e = &g_klog[(start + i) % KLOG_RING_CAP];
    if (e->seq <= since) continue;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "seq", (double)e->seq);
    cJSON_AddNumberToObject(o, "ts",  (double)e->ts);
    cJSON_AddStringToObject(o, "line", e->line);
    cJSON_AddItemToArray(arr, o);
  }
  cJSON_AddNumberToObject(r, "lastSeq", (double)g_klog_seq);
  cJSON_AddNumberToObject(r, "count",   (double)g_klog_count);
  cJSON_AddBoolToObject  (r, "paused",  g_klog_paused ? 1 : 0);
  pthread_mutex_unlock(&g_klog_lock);
  return serve_json_obj(c, r, MHD_HTTP_OK);
}


static enum MHD_Result
klogs_clear(struct MHD_Connection *c) {
  pthread_mutex_lock(&g_klog_lock);
  g_klog_count = 0;
  g_klog_head  = 0;
  pthread_mutex_unlock(&g_klog_lock);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", 1);
  return serve_json_obj(c, r, MHD_HTTP_OK);
}


static enum MHD_Result
klogs_pause(struct MHD_Connection *c, int paused) {
  pthread_mutex_lock(&g_klog_lock);
  g_klog_paused = paused ? 1 : 0;
  pthread_mutex_unlock(&g_klog_lock);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok",     1);
  cJSON_AddBoolToObject(r, "paused", paused ? 1 : 0);
  return serve_json_obj(c, r, MHD_HTTP_OK);
}


enum MHD_Result
dashboards_klogs_request(struct MHD_Connection *c, const char *url) {
  if (!strcmp(url, "/api/klogs"))         return klogs_get(c);
  if (!strcmp(url, "/api/klogs/clear"))   return klogs_clear(c);
  if (!strcmp(url, "/api/klogs/pause"))   return klogs_pause(c, 1);
  if (!strcmp(url, "/api/klogs/resume"))  return klogs_pause(c, 0);
  if (!strcmp(url, "/api/klogs/status")) {
    pthread_mutex_lock(&g_klog_lock);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject  (r, "paused",  g_klog_paused ? 1 : 0);
    cJSON_AddNumberToObject(r, "count",   (double)g_klog_count);
    cJSON_AddNumberToObject(r, "lastSeq", (double)g_klog_seq);
    pthread_mutex_unlock(&g_klog_lock);
    return serve_json_obj(c, r, MHD_HTTP_OK);
  }
  return MHD_NO;
}



extern int sceKernelGetCpuTemperature(int *out);
extern int sceKernelGetSocSensorTemperature(int channel, int *out);

static long
read_uptime_seconds(void) {
  struct timeval boottime;
  size_t sz = sizeof(boottime);
  if (sysctlbyname("kern.boottime", &boottime, &sz, NULL, 0) != 0) return -1;
  time_t now = time(NULL);
  return (long)(now - boottime.tv_sec);
}


enum MHD_Result
dashboards_stats_request(struct MHD_Connection *c) {
  cJSON *r = cJSON_CreateObject();
  int    cpu_c = -1, soc_c = -1;
  int    cpu_ok = (sceKernelGetCpuTemperature(&cpu_c) == 0);
  int    soc_ok = (sceKernelGetSocSensorTemperature(0, &soc_c) == 0);
  if (cpu_ok && (cpu_c < 0 || cpu_c > 130)) cpu_ok = 0;
  if (soc_ok && (soc_c < 0 || soc_c > 130)) soc_ok = 0;

  if (cpu_ok) cJSON_AddNumberToObject(r, "cpuC", cpu_c);
  if (soc_ok) cJSON_AddNumberToObject(r, "socC", soc_c);
  int hottest = cpu_ok ? cpu_c : -1;
  if (soc_ok && soc_c > hottest) hottest = soc_c;
  if (hottest >= 0) cJSON_AddNumberToObject(r, "hottestC", hottest);

  long up = read_uptime_seconds();
  if (up >= 0) cJSON_AddNumberToObject(r, "uptimeSec", (double)up);

  cJSON_AddNumberToObject(r, "klogBuffered", (double)g_klog_count);
  cJSON_AddBoolToObject  (r, "klogPaused",   g_klog_paused ? 1 : 0);

  /* PID + process name placeholder so the dashboard can show "loader"
     entries in a "what's running" card. Cheap, no kernel R/W. */
  cJSON_AddNumberToObject(r, "loaderPid", (double)getpid());

  cJSON_AddBoolToObject  (r, "ok", 1);
  return serve_json_obj(c, r, MHD_HTTP_OK);
}
