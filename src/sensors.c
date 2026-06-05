
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <microhttpd.h>

#include "sensors.h"
#include "sys.h"
#include "third_party/cJSON.h"
#include "websrv.h"


/* ── confirmed-good signatures (hwinfo sample + ea-fps) ───────────── */
int  sceKernelGetCpuTemperature(int *out_celsius);
int  sceKernelGetSocSensorTemperature(int channel, int *out_celsius);
long sceKernelGetCpuFrequency(void);
int  sceKernelGetCurrentFanDuty(uint16_t *out_duty, void *scratch);
int  sceKernelGetHwModelName(char *out);
int  sceKernelGetHwSerialNumber(char *out);

/* ── single-pointer probes (conservative) ─────────────────────────── */
int  sceKernelGetBasicProductShape(int *out);
int  sceKernelGetCpumode(void);               /* no args, returns int  */
int  sceKernelIccGetPowerNumberOfBootShutdown(uint64_t *out);
int  sceKernelIccGetPowerOperatingTime(uint64_t *out);

/* ── per-core CPU usage ────────────────────────────────────────────── */
int  sceKernelGetCpuUsageAll(int *per_core_pct, int *count_out);

/* ── SoC power (buffer + reserved-double, from phu decompilation) ──── */
int  sceKernelGetSocPowerConsumption(uint64_t *out, double reserved);


static enum MHD_Result
serve_json_free(struct MHD_Connection *conn, cJSON *obj) {
  char *txt = cJSON_PrintUnformatted(obj);
  cJSON_Delete(obj);
  if(!txt) {
    const char *err = "{\"ok\":false,\"error\":\"alloc\"}";
    struct MHD_Response *r =
        MHD_create_response_from_buffer(strlen(err), (void*)err,
                                        MHD_RESPMEM_PERSISTENT);
    enum MHD_Result rc = websrv_queue_response(conn, 500, r);
    MHD_destroy_response(r);
    return rc;
  }
  struct MHD_Response *r =
      MHD_create_response_from_buffer(strlen(txt), txt,
                                      MHD_RESPMEM_MUST_FREE);
  MHD_add_response_header(r, MHD_HTTP_HEADER_CONTENT_TYPE,
                          "application/json");
  MHD_add_response_header(r, MHD_HTTP_HEADER_CACHE_CONTROL, "no-cache");
  enum MHD_Result rc = websrv_queue_response(conn, 200, r);
  MHD_destroy_response(r);
  return rc;
}


enum MHD_Result
sensors_request(struct MHD_Connection *conn) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", 1);

  /* ── hardware identity ──────────────────────────────────────────── */
  {
    char model[256] = {0};
    char serial[256] = {0};
    if(sceKernelGetHwModelName(model)   == 0) cJSON_AddStringToObject(root, "model",  model);
    if(sceKernelGetHwSerialNumber(serial)== 0) cJSON_AddStringToObject(root, "serial", serial);
  }

  {
    unsigned int fw = sys_get_firmware_version();
    char fwstr[16];
    snprintf(fwstr, sizeof(fwstr), "0x%08X", fw);
    cJSON_AddStringToObject(root, "firmwareVersion", fwstr);
  }

  {
    int shape = -1;
    if(sceKernelGetBasicProductShape(&shape) == 0)
      cJSON_AddNumberToObject(root, "productShape", shape);
  }

  /* ── temperatures ───────────────────────────────────────────────── */
  {
    int v = -1;
    if(sceKernelGetCpuTemperature(&v) == 0 && v >= 0 && v <= 130)
      cJSON_AddNumberToObject(root, "cpuTempC", v);
    else
      cJSON_AddNullToObject(root, "cpuTempC");
  }

  /* SoC sensor channels 0-7; expose all; flag known channels */
  cJSON *soc = cJSON_CreateArray();
  for(int ch = 0; ch < 8; ch++) {
    int v = -1;
    int rc = sceKernelGetSocSensorTemperature(ch, &v);
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "channel", ch);
    if(rc == 0 && v >= 0 && v <= 130) {
      /* ch2 M.2: only valid if >=20 (empty slot returns 0) */
      int valid = (ch == 2) ? (v >= 20) : 1;
      if(valid) {
        cJSON_AddNumberToObject(entry, "tempC", v);
        cJSON_AddBoolToObject(entry, "present", 1);
      } else {
        cJSON_AddNullToObject(entry, "tempC");
        cJSON_AddBoolToObject(entry, "present", 0);
      }
    } else {
      cJSON_AddNullToObject(entry, "tempC");
      cJSON_AddBoolToObject(entry, "present", 0);
    }
    /* Label known channels */
    const char *label = NULL;
    if(ch == 0) label = "SoC";
    else if(ch == 1) label = "VRM/Board";
    else if(ch == 2) label = "M.2 NVMe";
    if(label) cJSON_AddStringToObject(entry, "label", label);
    cJSON_AddItemToArray(soc, entry);
  }
  cJSON_AddItemToObject(root, "socChannels", soc);

  /* ── performance ────────────────────────────────────────────────── */
  {
    long hz = sceKernelGetCpuFrequency();
    if(hz > 0)
      cJSON_AddNumberToObject(root, "cpuFreqMHz", (double)(hz / 1000000L));
    else
      cJSON_AddNullToObject(root, "cpuFreqMHz");
  }

  {
    int mode = sceKernelGetCpumode();
    cJSON_AddNumberToObject(root, "cpuMode", mode);
    /* 0=low/normal, 1=boost, 2=game — exact values TBD from live data */
    const char *mode_s = (mode == 0) ? "normal" :
                         (mode == 1) ? "boost"  :
                         (mode == 2) ? "game"   : "unknown";
    cJSON_AddStringToObject(root, "cpuModeStr", mode_s);
  }

  /* ── fan ────────────────────────────────────────────────────────── */
  {
    uint16_t duty = 0;
    uint8_t  scratch[64] = {0};
    if(sceKernelGetCurrentFanDuty(&duty, scratch) == 0) {
      int pct = (int)duty;
      if(pct > 100) pct = (pct * 100) / 255;
      if(pct > 100) pct = 100;
      cJSON_AddNumberToObject(root, "fanDutyPct", pct);
      cJSON_AddNumberToObject(root, "fanDutyRaw", (int)duty);
    } else {
      cJSON_AddNullToObject(root, "fanDutyPct");
    }
  }

  /* ── per-core CPU usage ─────────────────────────────────────────── */
  {
    int per_core[8] = {-1,-1,-1,-1,-1,-1,-1,-1};
    int count = 0;
    int rc = sceKernelGetCpuUsageAll(per_core, &count);
    if(rc == 0 && count > 0 && count <= 8) {
      cJSON *arr = cJSON_CreateArray();
      for(int i = 0; i < count; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(per_core[i]));
      cJSON_AddItemToObject(root, "cpuUsagePerCore", arr);
      cJSON_AddNumberToObject(root, "cpuCoreCount", count);
    } else {
      cJSON_AddNullToObject(root, "cpuUsagePerCore");
      cJSON_AddNumberToObject(root, "cpuUsageRc", rc);
    }
  }

  /* ── SoC power consumption ──────────────────────────────────────── */
  {
    uint64_t power_buf[16] = {0};
    int rc = sceKernelGetSocPowerConsumption(power_buf, 0.0);
    if(rc == 0) {
      /* Expose first 4 raw values; unit TBD from live data.
         Community finding: power_buf[0] is likely milliwatts.    */
      cJSON *raw = cJSON_CreateArray();
      for(int i = 0; i < 4; i++)
        cJSON_AddItemToArray(raw, cJSON_CreateNumber((double)power_buf[i]));
      cJSON_AddItemToObject(root, "socPowerRaw", raw);
      /* Guess: divide by 1000 for watts if value is reasonable */
      if(power_buf[0] > 0 && power_buf[0] < 500000)
        cJSON_AddNumberToObject(root, "socPowerWGuess",
                                (double)power_buf[0] / 1000.0);
    } else {
      cJSON_AddNullToObject(root, "socPowerRaw");
      cJSON_AddNumberToObject(root, "socPowerRc", rc);
    }
  }

  /* ── system lifetime counters ───────────────────────────────────── */
  {
    uint64_t op_time = 0;
    if(sceKernelIccGetPowerOperatingTime(&op_time) == 0) {
      cJSON_AddNumberToObject(root, "operatingTimeSec", (double)op_time);
    } else {
      cJSON_AddNullToObject(root, "operatingTimeSec");
    }
  }

  {
    uint64_t boots = 0;
    if(sceKernelIccGetPowerNumberOfBootShutdown(&boots) == 0) {
      cJSON_AddNumberToObject(root, "bootShutdownCount", (double)boots);
    } else {
      cJSON_AddNullToObject(root, "bootShutdownCount");
    }
  }

  return serve_json_free(conn, root);
}
