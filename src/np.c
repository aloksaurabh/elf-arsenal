
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <microhttpd.h>

#include "backup.h"
#include "np.h"
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
serve_error(struct MHD_Connection *conn, unsigned int status, const char *msg) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "ok", 0);
  cJSON_AddStringToObject(o, "error", msg);
  enum MHD_Result ret = serve_json(conn, status, o);
  cJSON_Delete(o);
  return ret;
}


static enum MHD_Result
info_request(struct MHD_Connection *conn) {
  char name[24] = {0};
  uint32_t uid = sys_get_foreground_user(name, sizeof(name));

  cJSON *r = cJSON_CreateObject();
  cJSON_AddNumberToObject(r, "userId", (double)uid);
  if(uid) {
    char hex[16];
    snprintf(hex, sizeof(hex), "0x%08X", uid);
    cJSON_AddStringToObject(r, "userIdHex", hex);
    cJSON_AddStringToObject(r, "userName",
                            name[0] ? name : "?");

    char path[160];
    /* np-fake-signin reads its own embedded templates and writes
       per-user state to /system_data/priv/home/<uid>/... */
    snprintf(path, sizeof(path),
             "/system_data/priv/home/%x/np/auth.dat", uid);
    cJSON_AddStringToObject(r, "authDat",   path);
    snprintf(path, sizeof(path),
             "/system_data/priv/home/%x/config.dat", uid);
    cJSON_AddStringToObject(r, "configDat", path);
    snprintf(path, sizeof(path),
             "/user/home/%x/np/account.dat", uid);
    cJSON_AddStringToObject(r, "accountDat", path);
    snprintf(path, sizeof(path),
             "/user/home/%x/np/token.dat", uid);
    cJSON_AddStringToObject(r, "tokenDat",  path);
    cJSON_AddBoolToObject(r,  "ok", 1);
  } else {
    cJSON_AddBoolToObject(r,  "ok", 0);
    cJSON_AddStringToObject(r, "error",
        "no foreground user — sign in to a profile first");
  }
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


static enum MHD_Result
fake_signin_request(struct MHD_Connection *conn) {
  if(sys_spawn_np_fake_signin() != 0) {
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
        "spawn failed — check the system log");
  }
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r,   "ok",      1);
  cJSON_AddStringToObject(r, "payload", "np-fake-signin.elf");
  cJSON_AddStringToObject(r, "note",
      "Spawned. Watch the system notification for progress, then "
      "reboot for the changes to take effect.");
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


/* GET /api/np/restore — spawn np-restore-account.elf detached. */
static enum MHD_Result
restore_request(struct MHD_Connection *conn) {
  if(sys_spawn_np_restore_account() != 0) {
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
        "spawn failed — check the system log");
  }
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r,   "ok",      1);
  cJSON_AddStringToObject(r, "payload", "np-restore-account.elf");
  cJSON_AddStringToObject(r, "note",
      "Spawned. The payload reads /system_data/priv/home/<uid>/config.dat "
      "and writes every field to the registry. Reboot when it finishes.");
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


enum MHD_Result
np_request(struct MHD_Connection *conn, const char *url) {
  if(!strcmp(url, "/api/np"))              return info_request(conn);
  if(!strcmp(url, "/api/np/info"))         return info_request(conn);
  if(!strcmp(url, "/api/np/fake-signin"))  return fake_signin_request(conn);
  if(!strcmp(url, "/api/np/restore"))      return restore_request(conn);
  return serve_error(conn, MHD_HTTP_NOT_FOUND, "no such endpoint");
}
