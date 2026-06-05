/* Elf Arsenal — Garlic Worker + SaveMgr Settings endpoints. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <microhttpd.h>

#include "garlic.h"
#include "sys.h"
#include "third_party/cJSON.h"
#include "websrv.h"


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
  if(!txt) return serve_buffer(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                               "application/json",
                               "{\"error\":\"alloc\"}", 17, 0);
  return serve_buffer(conn, status, "application/json", txt, strlen(txt), 1);
}


static enum MHD_Result
respond_state(struct MHD_Connection *conn) {
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r,   "ok",             1);
  cJSON_AddBoolToObject(r,   "workerRunning",  sys_garlic_worker_is_running());
  cJSON_AddBoolToObject(r,   "savemgrRunning", sys_garlic_savemgr_is_running());
  cJSON_AddNumberToObject(r, "pollInterval",   sys_garlic_get_poll_interval());
  cJSON_AddNumberToObject(r, "savemgrPort",    8082);
  cJSON_AddStringToObject(r, "configPath",     "/data/garlic/config.ini");
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


static enum MHD_Result
worker_request(struct MHD_Connection *conn) {
  const char *on = MHD_lookup_connection_value(conn,
                          MHD_GET_ARGUMENT_KIND, "on");
  if(on) {
    int want = strcmp(on, "0") != 0;
    sys_garlic_worker_set_enabled(want);
    /* Persist immediately so the choice survives reboots. */
    extern void config_save(void);
    config_save();
  }
  return respond_state(conn);
}


static enum MHD_Result
savemgr_request(struct MHD_Connection *conn) {
  if(!sys_garlic_savemgr_is_running())
    sys_garlic_savemgr_set_enabled(1);
  return respond_state(conn);
}


static enum MHD_Result
poll_request(struct MHD_Connection *conn) {
  const char *s = MHD_lookup_connection_value(conn,
                          MHD_GET_ARGUMENT_KIND, "seconds");
  if(s) {
    int n = atoi(s);
    if(n < 5)    n = 5;
    if(n > 3600) n = 3600;
    sys_garlic_set_poll_interval(n);
    extern void config_save(void);
    config_save();
  }
  return respond_state(conn);
}


enum MHD_Result
garlic_request(struct MHD_Connection *conn, const char *url) {
  if(!strcmp(url, "/api/garlic"))         return respond_state(conn);
  if(!strcmp(url, "/api/garlic/worker"))  return worker_request(conn);
  if(!strcmp(url, "/api/garlic/savemgr")) return savemgr_request(conn);
  if(!strcmp(url, "/api/garlic/poll"))    return poll_request(conn);
  cJSON *err = cJSON_CreateObject();
  cJSON_AddBoolToObject(err, "ok", 0);
  cJSON_AddStringToObject(err, "error", "no such endpoint");
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_NOT_FOUND, err);
  cJSON_Delete(err);
  return ret;
}
