
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <microhttpd.h>

#include "fan.h"
#include "third_party/cJSON.h"
#include "websrv.h"


int sceKernelGetCpuTemperature(int *out_celsius);
int sceKernelGetSocSensorTemperature(int sensor_id, int *out_celsius);


#define ICC_FAN_DEVICE     "/dev/icc_fan"

#define ICC_FAN_THRESHOLD_IOCTL 0xC01C8F07ul

#define ICC_FAN_GET_MANUAL_DUTY 0xC0068F06ul

#define FAN_MIN_C  30
#define FAN_MAX_C  90

#define FAN_REAPPLY_SEC 15

static atomic_int  g_pinned_threshold_c   = 0;  /* 0 = no pin yet */
static atomic_int  g_fan_watcher_started  = 0;


int
fan_pinned_threshold(void) {
  return atomic_load(&g_pinned_threshold_c);
}

void
fan_pin_threshold(int temp_c) {
  if(temp_c < FAN_MIN_C) temp_c = FAN_MIN_C;
  if(temp_c > FAN_MAX_C) temp_c = FAN_MAX_C;
  atomic_store(&g_pinned_threshold_c, temp_c);
}


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
fan_set_threshold(int temp_c, int *errno_out, unsigned char duty_out[6]) {
  if(temp_c < FAN_MIN_C) temp_c = FAN_MIN_C;
  if(temp_c > FAN_MAX_C) temp_c = FAN_MAX_C;
  if(errno_out) *errno_out = 0;

  int fd = open(ICC_FAN_DEVICE, O_RDONLY);
  if(fd < 0) {
    if(errno_out) *errno_out = errno;
    return -1;
  }

  /* Threshold byte at offset 5 — exact layout Elf Arsenal's daemon uses. */
  unsigned char data[10] = {0, 0, 0, 0, 0,
                            (unsigned char)temp_c,
                            0, 0, 0, 0};
  int rc = ioctl(fd, ICC_FAN_THRESHOLD_IOCTL, data);
  int eno = errno;

  /* Read the manual-duty register back for diagnostic display.
     Failures here are non-fatal — they don't change rc. */
  if(duty_out) {
    unsigned char duty[6] = {0};
    if(ioctl(fd, ICC_FAN_GET_MANUAL_DUTY, duty) == 0) {
      memcpy(duty_out, duty, 6);
    }
  }

  close(fd);
  if(rc < 0 && errno_out) *errno_out = eno;
  return rc;
}


static void*
fan_watcher_thread_fn(void *arg) {
  (void)arg;
  syscall(SYS_thr_set_name, -1, "fan-watcher");

  for(;;) {
    for(int i=0; i<FAN_REAPPLY_SEC; i++) sleep(1);

    int t = atomic_load(&g_pinned_threshold_c);
    if(t < FAN_MIN_C || t > FAN_MAX_C) continue;  /* nothing pinned yet */

    int eno = 0;
    int rc = fan_set_threshold(t, &eno, NULL);
    if(rc != 0) {
      /* Don't spam stdout/klog when the device is briefly unavailable;
         the next tick will retry. */
      printf("fan: re-apply rc=%d errno=%d (%s)\n",
             rc, eno, strerror(eno));
    }
  }
  return NULL;
}


void
fan_init(void) {
  if(atomic_exchange(&g_fan_watcher_started, 1)) return;

  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  pthread_create(&t, &a, fan_watcher_thread_fn, NULL);
  pthread_attr_destroy(&a);

  printf("fan: watcher started — pinned threshold re-applied every %ds\n",
         FAN_REAPPLY_SEC);
}


static enum MHD_Result
set_threshold_request(struct MHD_Connection *conn) {
  const char *temp_s = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND,
                                                   "temp");
  if(!temp_s) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                       "missing 'temp' argument (Celsius)");
  }
  int temp = atoi(temp_s);
  if(temp < FAN_MIN_C || temp > FAN_MAX_C) {
    char err[96];
    snprintf(err, sizeof(err), "temp must be %d..%d Celsius", FAN_MIN_C, FAN_MAX_C);
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, err);
  }

  int eno = 0;
  unsigned char duty[6] = {0};
  int rc = fan_set_threshold(temp, &eno, duty);

  cJSON *r = cJSON_CreateObject();
  if(rc != 0) {
    char errbuf[224];
    snprintf(errbuf, sizeof(errbuf),
             "/dev/icc_fan ioctl 0xC01C8F07 rejected the request "
             "(rc=%d errno=%d=%s). Make sure kstuff is loaded so "
             "/dev/icc_fan is accessible.",
             rc, eno, strerror(eno));
    cJSON_AddBoolToObject(r,    "ok", 0);
    cJSON_AddStringToObject(r,  "error", errbuf);
    cJSON_AddNumberToObject(r,  "ioctlResult", rc);
    cJSON_AddNumberToObject(r,  "errno", eno);
  } else {
    /* Pin the value so the watcher thread keeps re-applying it after
       the firmware resets fan state on app/game launches. */
    atomic_store(&g_pinned_threshold_c, temp);
    /* Persist the new threshold to /data/elf-arsenal/config.ini so it
       survives a redeploy of the payload. */
    extern void config_save(void);
    config_save();

    cJSON_AddBoolToObject(r,    "ok", 1);
    cJSON_AddNumberToObject(r,  "thresholdCelsius",    temp);
    cJSON_AddNumberToObject(r,  "thresholdFahrenheit", (temp * 9 / 5) + 32);
    cJSON_AddStringToObject(r,  "via",
        "icc_fan_threshold ioctl 0xC01C8F07 (Elf Arsenal-compatible path)");
    cJSON_AddNumberToObject(r,  "manualDuty", duty[0]);
    cJSON_AddBoolToObject(r,    "pinned", 1);
    cJSON_AddNumberToObject(r,  "reapplyEverySeconds", FAN_REAPPLY_SEC);
  }
  enum MHD_Result ret = serve_json(conn,
                                   rc == 0 ? MHD_HTTP_OK
                                           : MHD_HTTP_INTERNAL_SERVER_ERROR, r);
  cJSON_Delete(r);
  return ret;
}


static enum MHD_Result
temp_request(struct MHD_Connection *conn) {
  int cpu_c = -1;
  int soc_c = -1;
  int cpu_ok = (sceKernelGetCpuTemperature(&cpu_c) == 0);
  int soc_ok = (sceKernelGetSocSensorTemperature(0, &soc_c) == 0);

  /* Probe all SoC sensor channels. Channel 2 is the M.2 NVMe sensor on
     most PS5 revisions. Surface as m2C so the UI can show it directly. */
  int m2_c = -1, m2_ok = 0;
  for(int ch = 0; ch < 8; ch++) {
    int v = -1;
    if(sceKernelGetSocSensorTemperature(ch, &v) == 0 && v >= 0 && v <= 130) {
      /* Require ≥20 °C for M.2: empty slot returns 0 on most revisions. */
      if(ch == 2 && v >= 20) { m2_c = v; m2_ok = 1; }
    }
  }

  /* Sanity-clamp — the SDK occasionally returns huge values when the
     sensor isn't ready (~immediately after wake from suspend). */
  if(cpu_ok && (cpu_c < 0 || cpu_c > 130)) cpu_ok = 0;
  if(soc_ok && (soc_c < 0 || soc_c > 130)) soc_ok = 0;

  int hottest = cpu_ok ? cpu_c : -1;
  if(soc_ok && soc_c > hottest) hottest = soc_c;
  if(m2_ok  && m2_c  > hottest) hottest = m2_c;

  int threshold = atomic_load(&g_pinned_threshold_c);

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r,  "ok",         (cpu_ok || soc_ok));
  if(cpu_ok) cJSON_AddNumberToObject(r, "cpuC", cpu_c);
  if(soc_ok) cJSON_AddNumberToObject(r, "socC", soc_c);
  if(m2_ok)  cJSON_AddNumberToObject(r, "m2C",  m2_c);
  if(hottest >= 0) cJSON_AddNumberToObject(r, "hottestC", hottest);
  cJSON_AddNumberToObject(r, "thresholdC", threshold);
  cJSON_AddNumberToObject(r, "minC",       FAN_MIN_C);
  cJSON_AddNumberToObject(r, "maxC",       FAN_MAX_C);
  /* Soft "alert" thresholds for the UI gradient — bar turns yellow at
     warmC, red at hotC. Same numbers etaHEN's overlay uses. */
  cJSON_AddNumberToObject(r, "warmC", 65);
  cJSON_AddNumberToObject(r, "hotC",  80);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


#define FAN_CURVE_PATH "/data/elf-arsenal/fan_curve.json"

static enum MHD_Result
curve_get_request(struct MHD_Connection *conn) {
  cJSON *r = cJSON_CreateObject();
  FILE *f = fopen(FAN_CURVE_PATH, "r");
  if(f) {
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(n > 0 && n < 16384) {
      char *buf = malloc((size_t)n + 1);
      if(buf) {
        size_t got = fread(buf, 1, (size_t)n, f);
        buf[got] = 0;
        cJSON *parsed = cJSON_Parse(buf);
        free(buf);
        if(parsed) cJSON_AddItemToObject(r, "points", parsed);
      }
    }
    fclose(f);
  }
  if(!cJSON_GetObjectItem(r, "points")) {
    cJSON *empty = cJSON_CreateArray();
    cJSON_AddItemToObject(r, "points", empty);
  }
  cJSON_AddBoolToObject(r, "ok", 1);
  cJSON_AddStringToObject(r, "path", FAN_CURVE_PATH);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}

static enum MHD_Result
curve_set_request(struct MHD_Connection *conn) {
  cJSON *arr = cJSON_CreateArray();
  for(int i = 0; i < 8; i++) {
    char k[8]; snprintf(k, sizeof(k), "p%d", i);
    const char *v = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, k);
    if(!v) break;
    int t = 0, th = 0;
    if(sscanf(v, "%d,%d", &t, &th) != 2) continue;
    if(t  < 0   || t  > 110) continue;
    if(th < FAN_MIN_C || th > FAN_MAX_C) continue;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "tempC",      t);
    cJSON_AddNumberToObject(o, "thresholdC", th);
    cJSON_AddItemToArray(arr, o);
  }
  char *body = cJSON_PrintUnformatted(arr);
  cJSON_Delete(arr);

  mkdir("/data/elf-arsenal", 0755);
  FILE *f = fopen(FAN_CURVE_PATH, "w");
  int saved = 0;
  if(f && body) {
    fputs(body, f);
    fclose(f);
    saved = 1;
  } else if(f) {
    fclose(f);
  }
  free(body);

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r,   "ok",    saved ? 1 : 0);
  cJSON_AddStringToObject(r, "path",  FAN_CURVE_PATH);
  enum MHD_Result ret = serve_json(conn,
                                   saved ? MHD_HTTP_OK : MHD_HTTP_INTERNAL_SERVER_ERROR,
                                   r);
  cJSON_Delete(r);
  return ret;
}


enum MHD_Result
fan_request(struct MHD_Connection *conn, const char *url) {
  if(!strcmp(url, "/api/fan/set")) {
    return set_threshold_request(conn);
  }
  if(!strcmp(url, "/api/fan/temp")) {
    return temp_request(conn);
  }
  if(!strcmp(url, "/api/fan/curve")) {
    return curve_get_request(conn);
  }
  if(!strcmp(url, "/api/fan/curve/set")) {
    return curve_set_request(conn);
  }
  if(!strcmp(url, "/api/fan")) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "minCelsius", FAN_MIN_C);
    cJSON_AddNumberToObject(r, "maxCelsius", FAN_MAX_C);
    cJSON_AddNumberToObject(r, "defaultCelsius", 60);
    cJSON_AddStringToObject(r, "device", ICC_FAN_DEVICE);
    enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
    cJSON_Delete(r);
    return ret;
  }
  return serve_error(conn, MHD_HTTP_NOT_FOUND, "no such endpoint");
}
