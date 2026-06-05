/* Elf Arsenal — EchoStretch/ps5-app-dumper bridge. */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <microhttpd.h>

#include "dumper.h"
#include "sys.h"
#include "third_party/cJSON.h"
#include "websrv.h"


static const char DUMPER_CONFIG_TEMPLATE[] =
"; PS5 App Dumper Config\n"
"\n"
"; === Decrypt App ===\n"
"; enable_decrypter = 1  -> decrypt ELF files (default)\n"
"; enable_decrypter = 0  -> disable decryption\n"
"enable_decrypter = 1\n"
"\n"
"; === Backport Options PS4/PS5 ===\n"
"; enable_backport = 1 -> enable SDK patching (default)\n"
"; enable_backport = 0 -> disable SDK patching\n"
"; ps4_backport_level = 1-6 -> predefined SDK pair (default: 4 (PS4 9.00) )\n"
"; ps5_backport_level = 1-10 -> predefined SDK pair (default: 1 (PS5 1.00) )\n"
"; >>>> BACKPORTING IS FOR ADVANCED USERS MAY NOT WORK <<<<\n"
"enable_backport = 0\n"
"ps4_backport_level = 4\n"
"ps5_backport_level = 1\n"
"\n"
"; === FSELF Files ===\n"
"; enable_elf2fself = 1 -> enable fself ELF files (default)\n"
"; enable_elf2fself = 0 -> disable fself\n"
"enable_elf2fself = 0\n"
"\n"
"; === Logging ===\n"
"; enable_logging = 1 -> write log.txt (default)\n"
"; enable_logging = 0 -> disable logging\n"
"enable_logging = 1\n"
"\n"
"; === PS4 Split Mode ===\n"
"; 0 = no split (CUSAxxxxx/)\n"
"; 1 = app only (CUSAxxxxx-app/)\n"
"; 2 = patch only (CUSAxxxxx-patch/)\n"
"; 3 = both split (CUSAxxxxx-app/ + CUSAxxxxx-patch/)\n"
"split=3\n"
"\n"
"; === Resume / skip-existing ===\n"
"; skip_existing = 1 -> if a destination file already\n"
";                       exists with size matching the\n"
";                       source, leave it alone. Lets\n"
";                       you re-run the dumper after a\n"
";                       USB unplug / power loss without\n"
";                       re-copying everything.\n"
"; skip_existing = 0 -> always overwrite (default,\n"
";                       backwards-compatible behaviour).\n"
"skip_existing = 0\n";

/* Same set the dumper itself probes — see utils.c possible_mounts[]. */
static const char *DUMPER_USB_MOUNTS[] = {
  "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
  "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7",
};
#define DUMPER_USB_MOUNT_COUNT \
  ((int)(sizeof(DUMPER_USB_MOUNTS)/sizeof(DUMPER_USB_MOUNTS[0])))

static int
dumper_mount_is_writable(const char *root) {
  struct stat st;
  if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
  char probe[256];
  snprintf(probe, sizeof(probe), "%s/.sl_dumper_probe", root);
  int fd = open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return 0;
  int ok = (write(fd, "x", 1) == 1);
  close(fd);
  unlink(probe);
  return ok;
}

static int
dumper_seed_one(const char *root) {
  char hb[256], cfg[320];
  snprintf(hb,  sizeof(hb),  "%s/homebrew",               root);
  snprintf(cfg, sizeof(cfg), "%s/homebrew/PS5DumpRunner", root);
  /* Ignore mkdir failures — EEXIST is the common case, anything
     else surfaces below when we try to open the config file. */
  (void)mkdir(hb,  0777);
  (void)mkdir(cfg, 0777);
  char path[512];
  snprintf(path, sizeof(path), "%s/homebrew/PS5DumpRunner/config.ini", root);
  struct stat st;
  if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) return 0;
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0) {
    /* Race with the dumper itself? Re-check existence. */
    if (stat(path, &st) == 0) return 0;
    return -1;
  }
  size_t total = sizeof(DUMPER_CONFIG_TEMPLATE) - 1;
  size_t off   = 0;
  while (off < total) {
    ssize_t w = write(fd, DUMPER_CONFIG_TEMPLATE + off, total - off);
    if (w <= 0) { close(fd); unlink(path); return -1; }
    off += (size_t)w;
  }
  close(fd);
  return 1;
}

void
dumper_seed_configs(void) {
  for (int i = 0; i < DUMPER_USB_MOUNT_COUNT; i++) {
    const char *root = DUMPER_USB_MOUNTS[i];
    if (!dumper_mount_is_writable(root)) continue;
    int rc = dumper_seed_one(root);
    if (rc == 1) {
      printf("dumper: seeded config at %s/homebrew/PS5DumpRunner/config.ini\n", root);
    }
  }
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
serve_json(struct MHD_Connection *conn, unsigned int status, cJSON *o) {
  char *txt = cJSON_PrintUnformatted(o);
  if(!txt) return serve_buffer(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                               "application/json",
                               "{\"error\":\"alloc\"}", 17, 0);
  return serve_buffer(conn, status, "application/json", txt, strlen(txt), 1);
}


static enum MHD_Result
run_request(struct MHD_Connection *conn) {
  if(sys_spawn_app_dumper() != 0) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddBoolToObject(e,   "ok", 0);
    cJSON_AddStringToObject(e, "error",
        "spawn failed — check the system log");
    enum MHD_Result ret = serve_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, e);
    cJSON_Delete(e);
    return ret;
  }
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r,   "ok", 1);
  cJSON_AddStringToObject(r, "payload", "ps5-app-dumper.elf");
  cJSON_AddStringToObject(r, "note",
      "Spawned. The dumper auto-detects the first writable USB drive "
      "and walks /mnt/sandbox/pfsmnt copying every mounted app/patch "
      "into <usb>/PS5/<TITLE_ID>/. Watch the system notifications. "
      "To enable skip-existing resume, edit "
      "<usb>/homebrew/PS5DumpRunner/config.ini and set skip_existing=1.");
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


static enum MHD_Result
seed_config_request(struct MHD_Connection *conn) {
  cJSON *r     = cJSON_CreateObject();
  cJSON *arr   = cJSON_AddArrayToObject(r, "mounts");
  int created  = 0;
  int existing = 0;
  int skipped  = 0;
  int errored  = 0;

  for (int i = 0; i < DUMPER_USB_MOUNT_COUNT; i++) {
    const char *root = DUMPER_USB_MOUNTS[i];
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "root", root);

    if (!dumper_mount_is_writable(root)) {
      cJSON_AddStringToObject(e, "status", "not-writable");
      skipped++;
    } else {
      int rc = dumper_seed_one(root);
      if (rc == 1) {
        cJSON_AddStringToObject(e, "status", "created");
        created++;
      } else if (rc == 0) {
        cJSON_AddStringToObject(e, "status", "already-exists");
        existing++;
      } else {
        cJSON_AddStringToObject(e, "status", "error");
        errored++;
      }
    }
    cJSON_AddItemToArray(arr, e);
  }

  cJSON_AddBoolToObject  (r, "ok",       errored == 0);
  cJSON_AddNumberToObject(r, "created",  created);
  cJSON_AddNumberToObject(r, "existing", existing);
  cJSON_AddNumberToObject(r, "skipped",  skipped);
  cJSON_AddNumberToObject(r, "errored",  errored);
  enum MHD_Result ret = serve_json(conn,
      errored == 0 ? MHD_HTTP_OK : MHD_HTTP_INTERNAL_SERVER_ERROR, r);
  cJSON_Delete(r);
  return ret;
}


static int
dumper_parse_int_kv(const char *body, const char *key, int defv) {
  size_t klen = strlen(key);
  const char *p = body;
  while (*p) {
    /* skip leading spaces */
    while (*p == ' ' || *p == '\t') p++;
    /* skip comment / blank lines */
    if (*p == '#' || *p == ';' || *p == '\n' || *p == '\r' || !*p) {
      while (*p && *p != '\n') p++;
      if (*p) p++;
      continue;
    }
    if (!strncmp(p, key, klen)) {
      const char *q = p + klen;
      while (*q == ' ' || *q == '\t') q++;
      if (*q == '=') {
        q++;
        while (*q == ' ' || *q == '\t') q++;
        if (*q >= '0' && *q <= '9') return atoi(q);
      }
    }
    while (*p && *p != '\n') p++;
    if (*p) p++;
  }
  return defv;
}

/* Read a USB's config.ini (if present) into a cJSON snapshot. */
static cJSON *
dumper_read_config_snapshot(const char *root) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "root", root);

  char path[512];
  snprintf(path, sizeof(path), "%s/homebrew/PS5DumpRunner/config.ini", root);
  cJSON_AddStringToObject(o, "path", path);

  FILE *f = fopen(path, "r");
  if (!f) {
    cJSON_AddBoolToObject(o, "exists", 0);
    return o;
  }
  cJSON_AddBoolToObject(o, "exists", 1);

  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0 || n > 32 * 1024) { fclose(f); return o; }
  char *buf = malloc((size_t)n + 1);
  if (!buf) { fclose(f); return o; }
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = 0;

  cJSON_AddNumberToObject(o, "skip_existing",
      dumper_parse_int_kv(buf, "skip_existing",       0));
  cJSON_AddNumberToObject(o, "enable_decrypter",
      dumper_parse_int_kv(buf, "enable_decrypter",    1));
  cJSON_AddNumberToObject(o, "enable_elf2fself",
      dumper_parse_int_kv(buf, "enable_elf2fself",    0));
  cJSON_AddNumberToObject(o, "enable_backport",
      dumper_parse_int_kv(buf, "enable_backport",     0));
  cJSON_AddNumberToObject(o, "ps4_backport_level",
      dumper_parse_int_kv(buf, "ps4_backport_level",  4));
  cJSON_AddNumberToObject(o, "ps5_backport_level",
      dumper_parse_int_kv(buf, "ps5_backport_level",  1));
  cJSON_AddNumberToObject(o, "enable_logging",
      dumper_parse_int_kv(buf, "enable_logging",      1));
  cJSON_AddNumberToObject(o, "split",
      dumper_parse_int_kv(buf, "split",               3));

  free(buf);
  return o;
}

static enum MHD_Result
list_configs_request(struct MHD_Connection *conn) {
  cJSON *r   = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(r, "mounts");
  for (int i = 0; i < DUMPER_USB_MOUNT_COUNT; i++) {
    const char *root = DUMPER_USB_MOUNTS[i];
    if (!dumper_mount_is_writable(root)) continue;
    cJSON_AddItemToArray(arr, dumper_read_config_snapshot(root));
  }
  cJSON_AddBoolToObject(r, "ok", 1);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}

/* Helper: read int query arg with default. */
static int
qs_int(struct MHD_Connection *conn, const char *key, int defv) {
  const char *v = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, key);
  if (!v || !v[0]) return defv;
  /* Defensive clamp at parse time so a malicious / malformed
     query string can't make us write garbage values into the file. */
  int n = atoi(v);
  return n;
}

static enum MHD_Result
set_config_request(struct MHD_Connection *conn) {
  const char *root = MHD_lookup_connection_value(
      conn, MHD_GET_ARGUMENT_KIND, "root");
  if (!root || !root[0]) {
    return serve_json(conn, MHD_HTTP_BAD_REQUEST,
        cJSON_Parse("{\"ok\":false,\"error\":\"missing root\"}"));
  }
  /* Validate root against our known list to prevent path injection. */
  int allowed = 0;
  for (int i = 0; i < DUMPER_USB_MOUNT_COUNT; i++) {
    if (!strcmp(root, DUMPER_USB_MOUNTS[i])) { allowed = 1; break; }
  }
  if (!allowed) {
    return serve_json(conn, MHD_HTTP_BAD_REQUEST,
        cJSON_Parse("{\"ok\":false,\"error\":\"bad root\"}"));
  }
  if (!dumper_mount_is_writable(root)) {
    return serve_json(conn, MHD_HTTP_PRECONDITION_FAILED,
        cJSON_Parse("{\"ok\":false,\"error\":\"mount not writable\"}"));
  }

  /* Pull each value, clamp to {0,1} (or known ranges) for safety. */
  int skip   = qs_int(conn, "skip_existing",       0) ? 1 : 0;
  int decr   = qs_int(conn, "enable_decrypter",    1) ? 1 : 0;
  int e2f    = qs_int(conn, "enable_elf2fself",    0) ? 1 : 0;
  int bp     = qs_int(conn, "enable_backport",     0) ? 1 : 0;
  int p4lvl  = qs_int(conn, "ps4_backport_level",  4);
  int p5lvl  = qs_int(conn, "ps5_backport_level",  1);
  int log    = qs_int(conn, "enable_logging",      1) ? 1 : 0;
  int split  = qs_int(conn, "split",               3);
  if (p4lvl < 1 || p4lvl > 6)  p4lvl = 4;
  if (p5lvl < 1 || p5lvl > 10) p5lvl = 1;
  if (split < 0 || split > 3)  split = 3;

  char hb[256], cfg[320], path[512];
  snprintf(hb,   sizeof(hb),   "%s/homebrew",                       root);
  snprintf(cfg,  sizeof(cfg),  "%s/homebrew/PS5DumpRunner",         root);
  snprintf(path, sizeof(path), "%s/homebrew/PS5DumpRunner/config.ini", root);
  (void)mkdir(hb,  0777);
  (void)mkdir(cfg, 0777);

  /* Atomic-write via tmp + rename so a concurrent dumper read can't
     observe a half-written file. */
  char tmp[640];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return serve_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
        cJSON_Parse("{\"ok\":false,\"error\":\"open .tmp failed\"}"));
  }
  char body[4096];
  int  body_len = snprintf(body, sizeof(body),
      "; PS5 App Dumper Config (managed by Elf Arsenal)\n"
      "\n"
      "; === Decrypt App ===\n"
      "enable_decrypter = %d\n"
      "\n"
      "; === Backport Options PS4/PS5 ===\n"
      "; ps4_backport_level = 1-6 (default 4 = PS4 9.00)\n"
      "; ps5_backport_level = 1-10 (default 1 = PS5 1.00)\n"
      "; >>>> BACKPORTING IS FOR ADVANCED USERS — MAY NOT WORK <<<<\n"
      "enable_backport = %d\n"
      "ps4_backport_level = %d\n"
      "ps5_backport_level = %d\n"
      "\n"
      "; === FSELF Files ===\n"
      "enable_elf2fself = %d\n"
      "\n"
      "; === Logging ===\n"
      "enable_logging = %d\n"
      "\n"
      "; === PS4 Split Mode === (0 none, 1 app, 2 patch, 3 both)\n"
      "split=%d\n"
      "\n"
      "; === Resume / skip-existing ===\n"
      "skip_existing = %d\n",
      decr, bp, p4lvl, p5lvl, e2f, log, split, skip);
  ssize_t w = write(fd, body, (size_t)body_len);
  close(fd);
  if (w != body_len) {
    unlink(tmp);
    return serve_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
        cJSON_Parse("{\"ok\":false,\"error\":\"write failed\"}"));
  }
  if (rename(tmp, path) != 0) {
    unlink(tmp);
    return serve_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
        cJSON_Parse("{\"ok\":false,\"error\":\"rename failed\"}"));
  }

  /* Re-read so the response confirms what landed on disk. */
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", 1);
  cJSON_AddItemToObject(r, "saved", dumper_read_config_snapshot(root));
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


enum MHD_Result
dumper_request(struct MHD_Connection *conn, const char *url) {
  if(!strcmp(url, "/api/dumper/run"))         return run_request(conn);
  if(!strcmp(url, "/api/dumper/seed-config")) return seed_config_request(conn);
  if(!strcmp(url, "/api/dumper/configs"))     return list_configs_request(conn);
  if(!strcmp(url, "/api/dumper/config/set"))  return set_config_request(conn);

  cJSON *err = cJSON_CreateObject();
  cJSON_AddBoolToObject(err, "ok", 0);
  cJSON_AddStringToObject(err, "error", "no such endpoint");
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_NOT_FOUND, err);
  cJSON_Delete(err);
  return ret;
}
