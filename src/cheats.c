
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/syscall.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <microhttpd.h>
#include "zipread.h"

#include <ps5/kernel.h>

#include "cheats.h"
#include "ps5/http.h"
#include "ps5/notify.h"
#include "ps5/pt.h"
#include "third_party/cJSON.h"
#include "third_party/mc4/mc4decrypter.h"
#include "titleid.h"
#include "websrv.h"


/* Shared cheats tree — CheatRunner reads from this same path, so dropping
   a file here makes it visible in both UIs. */
#define CHEATS_DIR             "/data/cheatrunner/cheats"
#define CHEATS_SUBDIR_JSON     CHEATS_DIR "/json"
#define CHEATS_SUBDIR_SHN      CHEATS_DIR "/shn"
#define CHEATS_SUBDIR_MC4      CHEATS_DIR "/mc4"

#define PATCHES_DIR            "/data/elf-arsenal/patches"
#define PATCHES_SUBDIR_JSON    PATCHES_DIR "/json"
#define PATCHES_SUBDIR_SHN     PATCHES_DIR "/shn"
#define PATCHES_SUBDIR_MC4     PATCHES_DIR "/mc4"
#define PATCHES_SUBDIR_XML     PATCHES_DIR "/xml"

/* Bounded sizes mirroring etaHEN's CheatManager.hpp so a malformed
   cheat file can't produce unbounded allocations. */
#define MAX_CHEAT_FILEPATH_LEN   0x100   /* 256 — full path to cheat file  */
#define MAX_CHEAT_NAME           256     /* mod/cheat display name         */
#define MAX_CHEAT_TITLE_ID_LEN   32      /* PPSA12345 / CUSA00004          */
#define MAX_CHEAT_VERSION_LEN    20      /* "01.07" etc.                   */
#define MAX_CHEAT_GAMENAME_LEN   512     /* localised game title           */
#define MAX_JSON_BYTES           (4u*1024u*1024u)  /* 4 MB hard parse cap  */

#define CHEAT_FW_PTRACE_REQ      0x840u

#define CHEAT_MC_NAME_TOK_A      "Master Code"
#define CHEAT_MC_NAME_TOK_B      "Mastercode"
#define CHEAT_MC_DEP_NAME_TOK    "MC"

static atomic_int g_cheats_engine_enabled = 0;

int  cheats_engine_enabled(void)            { return atomic_load(&g_cheats_engine_enabled); }
void cheats_engine_set_enabled(int on)      {
  atomic_store(&g_cheats_engine_enabled, on ? 1 : 0);
  extern void config_save(void);
  config_save();
}

int sceSystemServiceGetAppIdOfRunningBigApp(void);

typedef struct app_info {
  uint32_t app_id;
  uint64_t unknown1;
  char     title_id[14];
  char     unknown2[0x3c];
} app_info_t;

int sceKernelGetAppInfo(pid_t pid, app_info_t *info);


/*  Helpers                                                               */

static enum MHD_Result
serve_buffer(struct MHD_Connection *conn, unsigned int status,
             const char *mime, void *data, size_t size, int free_after) {
  enum MHD_Result ret = MHD_NO;
  struct MHD_Response *resp;
  enum MHD_ResponseMemoryMode mode = free_after ? MHD_RESPMEM_MUST_FREE
                                                : MHD_RESPMEM_PERSISTENT;
  if((resp=MHD_create_response_from_buffer(size, data, mode))) {
    if(mime) {
      MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, mime);
    }
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-cache");
    ret = websrv_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
  } else if(free_after) {
    free(data);
  }
  return ret;
}


static enum MHD_Result
serve_json_object(struct MHD_Connection *conn, unsigned int status,
                  cJSON *obj) {
  char *txt = cJSON_PrintUnformatted(obj);
  if(!txt) {
    return serve_buffer(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                        "application/json",
                        "{\"error\":\"alloc\"}", 17, 0);
  }
  return serve_buffer(conn, status, "application/json", txt, strlen(txt), 1);
}


static enum MHD_Result
serve_error(struct MHD_Connection *conn, unsigned int status,
            const char *message) {
  cJSON *obj = cJSON_CreateObject();
  cJSON_AddBoolToObject(obj, "ok", 0);
  cJSON_AddStringToObject(obj, "error", message ? message : "error");
  enum MHD_Result ret = serve_json_object(conn, status, obj);
  cJSON_Delete(obj);
  return ret;
}


/* A bare title id is exactly CUSA##### or PPSA##### — 9 chars, all alnum.
   We use this for the running-game match. */
static int
is_safe_title_id(const char *s) {
  if(!s || !*s) return 0;
  size_t n = strlen(s);
  if(n < 9 || n > 16) return 0;
  for(size_t i=0; i<n; i++) {
    char c = s[i];
    if(!((c>='A'&&c<='Z') || (c>='a'&&c<='z') || (c>='0'&&c<='9') ||
         c=='-' || c=='_')) {
      return 0;
    }
  }
  return 1;
}


static int
extract_title_id_prefix(const char *filename, char *out, size_t out_size) {
  if(!filename || out_size < 10) return 0;
  return title_id_normalize(filename, out);
}


/* Recognised cheat file extensions, in priority order. */
static int
recognised_cheat_extension(const char *name) {
  size_t n = strlen(name);
  if(n > 5 && !strcasecmp(name + n - 5, ".json")) return 1;
  if(n > 4 && !strcasecmp(name + n - 4, ".shn"))  return 2;
  if(n > 4 && !strcasecmp(name + n - 4, ".mc4"))  return 3;
  return 0;
}


static int
hex_nibble(char c) {
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return 10 + c - 'a';
  if(c >= 'A' && c <= 'F') return 10 + c - 'A';
  return -1;
}


static int
parse_hex_bytes(const char *s, uint8_t **out, size_t *out_len) {
  size_t cap = 32;
  size_t len = 0;
  uint8_t *buf = malloc(cap);
  int high = -1;

  for(const char *p=s; *p; p++) {
    if(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '-' ||
       *p == ',' || *p == ':') continue;
    int n = hex_nibble(*p);
    if(n < 0) { free(buf); return -1; }
    if(high < 0) {
      high = n;
    } else {
      if(len + 1 > cap) { cap *= 2; buf = realloc(buf, cap); }
      buf[len++] = (uint8_t)((high << 4) | n);
      high = -1;
    }
  }
  if(high >= 0) { free(buf); return -1; }
  *out = buf;
  *out_len = len;
  return 0;
}


static uint64_t
parse_offset(const char *s) {
  if(!s) return 0;
  while(*s == ' ' || *s == '\t') s++;
  /* Cheats use bare hex (no 0x). Accept either. */
  if(s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
  return (uint64_t)strtoull(s, NULL, 16);
}


/*  Running-game detection                                                */

static pid_t
find_pid_for_app_id(uint32_t app_id) {
  int mib[4] = {1, 14, 8, 0};
  size_t buf_size = 0;
  if(sysctl(mib, 4, NULL, &buf_size, NULL, 0) != 0) return -1;
  uint8_t *buf = malloc(buf_size);
  if(!buf) return -1;
  if(sysctl(mib, 4, buf, &buf_size, NULL, 0) != 0) { free(buf); return -1; }

  pid_t result = -1;
  app_info_t info;
  for(uint8_t *ptr=buf; ptr<buf+buf_size; ) {
    int ki_structsize = *(int*)ptr;
    pid_t pid = *(pid_t*)&ptr[72];
    ptr += ki_structsize;
    memset(&info, 0, sizeof(info));
    if(sceKernelGetAppInfo(pid, &info) == 0 && info.app_id == app_id) {
      result = pid;
      break;
    }
  }
  free(buf);
  return result;
}


static int
get_running_game(pid_t *out_pid, char *out_title, size_t title_size,
                 intptr_t *out_base) {
  int app_id = sceSystemServiceGetAppIdOfRunningBigApp();
  if(app_id <= 0) return -1;
  pid_t pid = find_pid_for_app_id((uint32_t)app_id);
  if(pid <= 0) return -1;
  app_info_t info;
  memset(&info, 0, sizeof(info));
  if(sceKernelGetAppInfo(pid, &info) != 0) return -1;
  if(out_title) {
    size_t n = strnlen(info.title_id, sizeof(info.title_id));
    if(n >= title_size) n = title_size - 1;
    memcpy(out_title, info.title_id, n);
    out_title[n] = 0;
  }
  if(out_base) *out_base = kernel_dynlib_mapbase_addr(pid, 0);
  if(out_pid) *out_pid = pid;
  return 0;
}


int
cheats_game_running(void) {
  return get_running_game(NULL, NULL, 0, NULL) == 0 ? 1 : 0;
}


/*  File I/O                                                              */

static void
ensure_cheats_dir(void) {
  mkdir("/data/elf-arsenal", 0755);
  mkdir(CHEATS_DIR,           0755);
  mkdir(CHEATS_SUBDIR_JSON,   0755);
  mkdir(CHEATS_SUBDIR_SHN,    0755);
  mkdir(CHEATS_SUBDIR_MC4,    0755);
  /* Auto-apply patches dir + per-format subdirs (same dual layout). */
  mkdir(PATCHES_DIR,          0755);
  mkdir(PATCHES_SUBDIR_JSON,  0755);
  mkdir(PATCHES_SUBDIR_SHN,   0755);
  mkdir(PATCHES_SUBDIR_MC4,   0755);
  mkdir(PATCHES_SUBDIR_XML,   0755);
}


void
cheats_init(void) {
  ensure_cheats_dir();
  chmod("/data/elf-arsenal",       0777);
  chmod(CHEATS_DIR,                 0777);
  chmod(CHEATS_SUBDIR_JSON,         0777);
  chmod(CHEATS_SUBDIR_SHN,          0777);
  chmod(CHEATS_SUBDIR_MC4,          0777);
  chmod(PATCHES_DIR,                0777);
  chmod(PATCHES_SUBDIR_JSON,        0777);
  chmod(PATCHES_SUBDIR_SHN,         0777);
  chmod(PATCHES_SUBDIR_MC4,         0777);
  chmod(PATCHES_SUBDIR_XML,         0777);
  uint32_t fw  = kernel_get_fw_version();
  uint32_t fwh = (fw >> 16) & 0xffffu;
  printf("cheats: ready (FW=0x%04x  write-path=%s)\n",
         fwh,
         fwh >= CHEAT_FW_PTRACE_REQ
           ? "ptrace+mprotect (required ≥ FW 8.40)"
           : "ptrace+mprotect (reference uses mdbg below FW 8.40)");
  printf("cheats: drop .json/.shn/.mc4 into %s (or %s/{json,shn,mc4}/) via FTP\n",
         CHEATS_DIR, CHEATS_DIR);
  printf("patches: auto-apply on game launch from %s (or %s/{json,shn,mc4}/)\n",
         PATCHES_DIR, PATCHES_DIR);
}



#include <pthread.h>

/* Forward declarations — the helpers below are defined later in this
   translation unit. */
static int   find_patch_file_for_title(const char *title_id, char *out,
                                       size_t out_size, int *kind_out);
static char* read_file_text(const char *path, size_t *out_len);
static char* shn_xml_to_json(const char *xml, size_t xml_len);
static char* mc4_decrypt_to_xml(const char *cipher, size_t cipher_len,
                                size_t *xml_size_out);
static int   resolve_module_base(pid_t pid, const char *module_name,
                                 intptr_t fallback_base, intptr_t *out_base);
static int   process_is_ps2_emu(pid_t pid);
static int   write_process_memory(pid_t pid, intptr_t addr,
                                  const uint8_t *data, size_t len);

static atomic_int g_patches_auto_enabled = 1;
static atomic_int g_patches_last_pid     = 0;
static atomic_int g_patches_last_mods    = 0;   /* mods applied to last game */
static atomic_int g_patches_total_writes = 0;   /* lifetime counter */

int  cheats_patches_auto_enabled(void)      { return atomic_load(&g_patches_auto_enabled); }
void cheats_patches_auto_set_enabled(int on) {
  atomic_store(&g_patches_auto_enabled, on ? 1 : 0);
  extern void config_save(void);
  config_save();
}
int cheats_patches_last_mod_count(void)    { return atomic_load(&g_patches_last_mods); }
int cheats_patches_total_writes(void)      { return atomic_load(&g_patches_total_writes); }


/* Resolve the effective write address for an SHN/MC4 patch entry.
   Probes both the relative (base+off) and absolute (off as-is) addresses
   and picks whichever already holds the patch's "on" bytes.  This handles
   the common case of SHN/MC4 files with absolute addresses that are missing
   an explicit <Absolute>true</Absolute> tag — the most frequent cause of
   60fps patches silently landing at the wrong address.  Falls back to the
   absolute address when neither probe matches. */
static intptr_t
resolve_patch_addr(pid_t pid, intptr_t mod_base, uint64_t off_u,
                   int explicit_absolute, int is_ps2,
                   const uint8_t *on_bytes, size_t byte_len) {
  intptr_t abs_addr = (intptr_t)off_u;
  intptr_t rel_addr = mod_base + (intptr_t)off_u;

  if(explicit_absolute || is_ps2)
    return abs_addr;

  /* Offsets below 0x200000 are unambiguously relative — no PS5 eboot loads
     that low.  Skip probing and return relative immediately. */
  if(off_u < 0x200000ULL || byte_len == 0 || byte_len > 128)
    return rel_addr;

  uint8_t probe[128];
  if(pt_copyout(pid, rel_addr, probe, byte_len) == 0 &&
     memcmp(probe, on_bytes, byte_len) == 0)
    return rel_addr;

  if(pt_copyout(pid, abs_addr, probe, byte_len) == 0 &&
     memcmp(probe, on_bytes, byte_len) == 0)
    return abs_addr;

  return abs_addr;
}


static int
cheats_apply_patches_for_running_game(int *out_mod_count) {
  pid_t pid = -1;
  intptr_t base = 0;
  char title[16], path[MAX_CHEAT_FILEPATH_LEN];
  int kind = 0, n_mods = 0, n_writes = 0;

  if(out_mod_count) *out_mod_count = 0;
  if(!atomic_load(&g_patches_auto_enabled)) return 0;
  if(!cheats_engine_enabled()) return 0;
  if(get_running_game(&pid, title, sizeof(title), &base) != 0) return 0;
  if(!find_patch_file_for_title(title, path, sizeof(path), &kind)) return 0;

  size_t len = 0;
  char *txt = read_file_text(path, &len);
  if(!txt) return 0;

  char *converted = NULL;
  if(kind == 2) {
    converted = shn_xml_to_json(txt, len);
    if(!converted) { free(txt); return 0; }
  } else if(kind == 3) {
    char *xml = mc4_decrypt_to_xml(txt, len, NULL);
    if(!xml) { free(txt); return 0; }
    converted = shn_xml_to_json(xml, strlen(xml));
    free(xml);
    if(!converted) { free(txt); return 0; }
  }

  cJSON *root = cJSON_Parse(converted ? converted : txt);
  free(converted); free(txt);
  if(!root) return 0;

  cJSON *mods = cJSON_GetObjectItem(root, "mods");
  if(!cJSON_IsArray(mods)) { cJSON_Delete(root); return 0; }

  int is_ps2 = process_is_ps2_emu(pid);
  if(pt_attach(pid) < 0) { cJSON_Delete(root); return 0; }

  cJSON *mod;
  cJSON_ArrayForEach(mod, mods) {
    cJSON *memory = cJSON_GetObjectItem(mod, "memory");
    if(!cJSON_IsArray(memory) || cJSON_GetArraySize(memory) == 0) continue;

    cJSON *mod_name_j = cJSON_GetObjectItem(mod, "module_name");
    const char *mod_name = (cJSON_IsString(mod_name_j) && mod_name_j->valuestring)
                              ? mod_name_j->valuestring : "";
    intptr_t mod_base = 0;
    if(resolve_module_base(pid, mod_name, base, &mod_base) != 0) continue;

    int wrote_any = 0;
    cJSON *m;
    cJSON_ArrayForEach(m, memory) {
      cJSON *off_j = cJSON_GetObjectItem(m, "offset");
      cJSON *on_j  = cJSON_GetObjectItem(m, "on");
      cJSON *abs_j = cJSON_GetObjectItem(m, "absolute");
      if(!cJSON_IsString(off_j) || !cJSON_IsString(on_j)) continue;
      uint64_t off = parse_offset(off_j->valuestring);
      int absolute = cJSON_IsTrue(abs_j) ? 1 : 0;
      uint8_t *on_bytes = NULL;
      size_t on_len = 0;
      if(parse_hex_bytes(on_j->valuestring, &on_bytes, &on_len) != 0) continue;
      if(on_len == 0) { free(on_bytes); continue; }
      intptr_t addr = resolve_patch_addr(pid, mod_base, off, absolute, is_ps2,
                                         on_bytes, on_len);
      if(write_process_memory(pid, addr, on_bytes, on_len) == 0) {
        n_writes++;
        wrote_any = 1;
      }
      free(on_bytes);
    }
    if(wrote_any) n_mods++;
  }

  pt_detach(pid, 0);
  cJSON_Delete(root);
  if(out_mod_count) *out_mod_count = n_mods;
  return n_writes;
}


static void*
patches_poll_thread(void *arg) {
  (void)arg;
  for(;;) {
    sleep(3);
    if(!atomic_load(&g_patches_auto_enabled)) continue;
    pid_t cur = -1;
    char title[16];
    intptr_t base = 0;
    if(get_running_game(&cur, title, sizeof(title), &base) != 0) {
      atomic_store(&g_patches_last_pid, 0);
      continue;
    }
    if((pid_t)atomic_load(&g_patches_last_pid) == cur) continue;
    int n = 0;
    int writes = cheats_apply_patches_for_running_game(&n);
    atomic_store(&g_patches_last_pid,  cur);
    atomic_store(&g_patches_last_mods, n);
    if(writes > 0) {
      atomic_fetch_add(&g_patches_total_writes, writes);
      printf("patches: applied %d mods (%d byte-stream writes) to %s\n",
             n, writes, title);
    }
  }
  return NULL;
}

void
cheats_patches_start_watcher(void) {
  pthread_t t;
  if(pthread_create(&t, NULL, patches_poll_thread, NULL) == 0) {
    pthread_detach(t);
    printf("patches: auto-apply watcher started (poll 3s)\n");
  } else {
    perror("patches: pthread_create");
  }
}


/*  SHN/MC4 → JSON conversion                                             */

/* Tiny string buffer for building JSON. */
typedef struct { char *buf; size_t len; size_t cap; } cb_t;

static void cb_putc(cb_t *b, char c) {
  if(b->len + 2 > b->cap) {
    b->cap = b->cap ? b->cap * 2 : 1024;
    b->buf = realloc(b->buf, b->cap);
  }
  b->buf[b->len++] = c;
  b->buf[b->len]   = 0;
}

static void cb_puts(cb_t *b, const char *s) {
  while(*s) cb_putc(b, *s++);
}

static void cb_puts_json(cb_t *b, const char *s, size_t n) {
  for(size_t i=0; i<n; i++) {
    char c = s[i];
    switch(c) {
      case '"':  cb_putc(b, '\\'); cb_putc(b, '"'); break;
      case '\\': cb_putc(b, '\\'); cb_putc(b, '\\'); break;
      case '\n': cb_putc(b, '\\'); cb_putc(b, 'n');  break;
      case '\r': cb_putc(b, '\\'); cb_putc(b, 'r');  break;
      case '\t': cb_putc(b, '\\'); cb_putc(b, 't');  break;
      default:
        if((unsigned char)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          cb_puts(b, buf);
        } else {
          cb_putc(b, c);
        }
    }
  }
}

static const char*
xml_find_attr(const char *node, const char *attr, size_t *len_out) {
  char needle[64];
  snprintf(needle, sizeof(needle), "%s=\"", attr);
  const char *p = strstr(node, needle);
  if(!p) {
    snprintf(needle, sizeof(needle), "%s='", attr);
    p = strstr(node, needle);
    if(!p) return NULL;
  }
  p += strlen(needle);
  const char *end = strpbrk(p, "\"'");
  if(!end) return NULL;
  *len_out = end - p;
  return p;
}

static const char*
xml_find_child(const char *node, const char *tag, size_t *len_out) {
  char open[64], close[64];
  snprintf(open,  sizeof(open),  "<%s>",  tag);
  snprintf(close, sizeof(close), "</%s>", tag);
  const char *p = strstr(node, open);
  if(!p) return NULL;
  p += strlen(open);
  const char *end = strstr(p, close);
  if(!end) return NULL;
  *len_out = end - p;
  return p;
}

static char*
mc4_decrypt_to_xml(const char *cipher, size_t cipher_len, size_t *xml_size_out) {
  if(!cipher || cipher_len == 0) return NULL;

  /* decrypt_data takes uint8_t*; it doesn't write past *size on its own,
     but we copy the input to a scratch buffer just in case. */
  uint8_t *scratch = malloc(cipher_len + 1);
  if(!scratch) return NULL;
  memcpy(scratch, cipher, cipher_len);
  scratch[cipher_len] = 0;

  size_t out_size = cipher_len;
  uint8_t *plain = decrypt_data(scratch, &out_size);
  free(scratch);
  if(!plain) return NULL;
  /* decrypt_data returns the same pointer on a base64 decode failure;
     guard against that landing right back here. */
  if(plain == (uint8_t*)scratch) return NULL;

  char *xml = malloc(out_size + 1);
  if(!xml) { free(plain); return NULL; }
  memcpy(xml, plain, out_size);
  xml[out_size] = 0;
  free(plain);

  /* In-place rewrite: <, >, " entity escapes → literal characters. */
  static const struct { const char *from; char to; } repl[] = {
    {"&lt;",      '<'},
    {"&gt;",      '>'},
    {"\\&quot;",  '"'},
    {"&quot;",    '"'},
  };
  size_t out_len = strlen(xml);
  for(size_t r = 0; r < sizeof(repl)/sizeof(repl[0]); r++) {
    const char *from = repl[r].from;
    size_t flen = strlen(from);
    char  to    = repl[r].to;
    char *src = xml, *dst = xml;
    while(*src) {
      if(strncmp(src, from, flen) == 0) {
        *dst++ = to;
        src += flen;
      } else {
        *dst++ = *src++;
      }
    }
    *dst = 0;
    out_len = (size_t)(dst - xml);
  }

  if(xml_size_out) *xml_size_out = out_len;
  return xml;
}


static char*
shn_xml_to_json(const char *xml, size_t xml_len) {
  (void)xml_len;
  cb_t out = {0};
  cb_puts(&out, "{\"name\":\"\",\"id\":\"\",\"version\":\"\",\"process\":\"eboot.bin\",\"mods\":[");

  size_t alen;
  const char *trainer = strstr(xml, "<Trainer");
  if(trainer) {
    const char *aend = strchr(trainer, '>');
    if(aend) {
      char tag[1024];
      size_t tn = aend - trainer;
      if(tn < sizeof(tag)) {
        memcpy(tag, trainer, tn); tag[tn] = 0;
        const char *v;
        if((v = xml_find_attr(tag, "Game", &alen)) ||
           (v = xml_find_attr(tag, "GameName", &alen))) {
          /* Splice in name */
          /* Already wrote the leading template — replace empty name. */
          char *needle = strstr(out.buf, "\"name\":\"\"");
          if(needle) {
            cb_t tmp = {0};
            cb_puts(&tmp, "\"name\":\"");
            cb_puts_json(&tmp, v, alen);
            cb_puts(&tmp, "\"");
            size_t prefix = needle - out.buf;
            cb_t merged = {0};
            cb_puts(&merged, "");
            for(size_t i=0; i<prefix; i++) cb_putc(&merged, out.buf[i]);
            cb_puts(&merged, tmp.buf);
            cb_puts(&merged, out.buf + prefix + 9);
            free(tmp.buf);
            free(out.buf);
            out = merged;
          }
        }
      }
    }
  }

  int first = 1;
  const char *cur = xml;
  while((cur = strstr(cur, "<Cheat ")) != NULL) {
    /* Closing > of the opening Cheat tag */
    const char *close = strchr(cur, '>');
    if(!close) break;
    size_t hdr_len = close - cur;
    char hdr[2048];
    if(hdr_len >= sizeof(hdr)) { cur = close; continue; }
    memcpy(hdr, cur, hdr_len); hdr[hdr_len] = 0;

    /* The Cheat may be self-closing or have a body — find </Cheat>. */
    const char *body_end = strstr(close, "</Cheat>");
    if(!body_end) break;
    const char *body_start = close + 1;

    if(!first) cb_putc(&out, ',');
    first = 0;

    cb_puts(&out, "{");
    /* Name */
    const char *t;
    cb_puts(&out, "\"name\":\"");
    if((t = xml_find_attr(hdr, "Text", &alen)) ||
       (t = xml_find_attr(hdr, "CheatName", &alen)) ||
       (t = xml_find_attr(hdr, "Name", &alen))) {
      cb_puts_json(&out, t, alen);
    }
    cb_puts(&out, "\",");
    /* Hint */
    cb_puts(&out, "\"hint\":");
    if((t = xml_find_attr(hdr, "Description", &alen)) && alen > 0) {
      cb_puts(&out, "\"");
      cb_puts_json(&out, t, alen);
      cb_puts(&out, "\"");
    } else {
      cb_puts(&out, "null");
    }
    cb_puts(&out, ",");
    /* Type — default to checkbox */
    cb_puts(&out, "\"type\":\"checkbox\",");
    cb_puts(&out, "\"memory\":[");

    int first_mem = 1;
    const char *line_cur = body_start;
    while(line_cur < body_end &&
          (line_cur = strstr(line_cur, "<Cheatline")) != NULL &&
          line_cur < body_end) {
      const char *line_close = strstr(line_cur, "</Cheatline>");
      const char *line_self  = strstr(line_cur, "/>");
      const char *line_end   = NULL;
      if(line_close && (!line_self || line_close < line_self)) {
        line_end = line_close + strlen("</Cheatline>");
      } else if(line_self) {
        line_end = line_self + 2;
      } else {
        break;
      }

      char chunk[4096];
      size_t cl = (size_t)(line_end - line_cur);
      if(cl >= sizeof(chunk)) { line_cur = line_end; continue; }
      memcpy(chunk, line_cur, cl); chunk[cl] = 0;

      size_t off_l, on_l, off2_l, abs_l;
      const char *off  = xml_find_child(chunk, "Offset",   &off_l);
      const char *on   = xml_find_child(chunk, "ValueOn",  &on_l);
      const char *off2 = xml_find_child(chunk, "ValueOff", &off2_l);
      const char *abs_ = xml_find_child(chunk, "Absolute", &abs_l);

      if(off && on && off2) {
        if(!first_mem) cb_putc(&out, ',');
        first_mem = 0;
        cb_puts(&out, "{\"offset\":\"");
        cb_puts_json(&out, off, off_l);
        cb_puts(&out, "\",\"on\":\"");
        cb_puts_json(&out, on, on_l);
        cb_puts(&out, "\",\"off\":\"");
        cb_puts_json(&out, off2, off2_l);
        cb_puts(&out, "\"");
        if(abs_ && abs_l > 0 &&
           (!strncasecmp(abs_, "true", 4) || abs_[0] == '1')) {
          cb_puts(&out, ",\"absolute\":true");
        }
        cb_puts(&out, "}");
      }
      line_cur = line_end;
    }
    cb_puts(&out, "]}");
    cur = body_end + strlen("</Cheat>");
  }

  cb_puts(&out, "]}");
  return out.buf;
}


static void
scan_one_cheats_dir(const char *dir, const char *title_id, size_t id_len,
                    int *best_kind, char *best_path, size_t best_path_size) {
  DIR *d = opendir(dir);
  if(!d) return;

  struct dirent *ent;
  while((ent = readdir(d))) {
    const char *name = ent->d_name;
    if(name[0] == '.') continue;
    if(strncasecmp(name, title_id, id_len) != 0) continue;
    char trailer = name[id_len];
    if(trailer != '.' && trailer != '_') continue;
    int k = recognised_cheat_extension(name);
    if(!k) continue;
    if(*best_kind == 0 || k < *best_kind) {
      int n = snprintf(best_path, best_path_size, "%s/%s", dir, name);
      if(n > 0 && (size_t)n < best_path_size) {
        *best_kind = k;
      }
      if(*best_kind == 1) break; /* JSON found — best possible, stop. */
    }
  }
  closedir(d);
}


static int
find_cheat_file_for_title(const char *title_id, char *out, size_t out_size,
                          int *kind_out) {
  if(!is_safe_title_id(title_id) || out_size < 32) return 0;
  size_t id_len = strlen(title_id);

  int  best_kind = 0;
  char best_path[MAX_CHEAT_FILEPATH_LEN] = {0};

  /* Scan flat root first (legacy / current default drop location)
     then per-format subdirs (reference layout). First JSON hit wins. */
  scan_one_cheats_dir(CHEATS_DIR,         title_id, id_len,
                      &best_kind, best_path, sizeof(best_path));
  if(best_kind != 1) {
    scan_one_cheats_dir(CHEATS_SUBDIR_JSON, title_id, id_len,
                        &best_kind, best_path, sizeof(best_path));
  }
  if(best_kind != 1 && best_kind != 2) {
    scan_one_cheats_dir(CHEATS_SUBDIR_SHN, title_id, id_len,
                        &best_kind, best_path, sizeof(best_path));
  }
  if(best_kind == 0) {
    scan_one_cheats_dir(CHEATS_SUBDIR_MC4, title_id, id_len,
                        &best_kind, best_path, sizeof(best_path));
  }
  if(!best_kind) return 0;

  size_t plen = strlen(best_path);
  if(plen + 1 > out_size) return 0;
  memcpy(out, best_path, plen + 1);
  if(kind_out) *kind_out = best_kind;
  return 1;
}


/* Same as find_cheat_file_for_title but searches the auto-apply
   patches/ tree instead of the cheats/ tree. */
static int
find_patch_file_for_title(const char *title_id, char *out, size_t out_size,
                          int *kind_out) {
  if(!is_safe_title_id(title_id) || out_size < 32) return 0;
  size_t id_len = strlen(title_id);
  int best_kind = 0;
  char best_path[MAX_CHEAT_FILEPATH_LEN] = {0};
  scan_one_cheats_dir(PATCHES_DIR,         title_id, id_len,
                      &best_kind, best_path, sizeof(best_path));
  if(best_kind != 1) {
    scan_one_cheats_dir(PATCHES_SUBDIR_JSON, title_id, id_len,
                        &best_kind, best_path, sizeof(best_path));
  }
  if(best_kind != 1 && best_kind != 2) {
    scan_one_cheats_dir(PATCHES_SUBDIR_SHN, title_id, id_len,
                        &best_kind, best_path, sizeof(best_path));
  }
  if(best_kind == 0) {
    scan_one_cheats_dir(PATCHES_SUBDIR_MC4, title_id, id_len,
                        &best_kind, best_path, sizeof(best_path));
  }
  if(!best_kind) return 0;
  size_t plen = strlen(best_path);
  if(plen + 1 > out_size) return 0;
  memcpy(out, best_path, plen + 1);
  if(kind_out) *kind_out = best_kind;
  return 1;
}


static char*
read_file_text(const char *path, size_t *out_len) {
  struct stat st;
  if(stat(path, &st) != 0) return NULL;
  if(st.st_size <= 0 || (size_t)st.st_size > MAX_JSON_BYTES) return NULL;
  int fd = open(path, O_RDONLY);
  if(fd < 0) return NULL;
  char *buf = malloc(st.st_size + 1);
  if(!buf) { close(fd); return NULL; }
  ssize_t n = read(fd, buf, st.st_size);
  close(fd);
  if(n != st.st_size) { free(buf); return NULL; }
  buf[n] = 0;
  if(out_len) *out_len = n;
  return buf;
}


static int
write_file_bytes(const char *path, const void *data, size_t len) {
  ensure_cheats_dir();
  int fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, 0644);
  if(fd < 0) return -1;
  ssize_t n = write(fd, data, len);
  close(fd);
  return (n == (ssize_t)len) ? 0 : -1;
}


/*  Cheat application                                                     */

#define ROUND_PG_DOWN(x) ((uintptr_t)(x) & ~(uintptr_t)0x3fff)
#define ROUND_PG_UP(x)   (((uintptr_t)(x) + 0x3fff) & ~(uintptr_t)0x3fff)


static int
write_process_memory(pid_t pid, intptr_t addr, const uint8_t *data,
                     size_t len) {
  intptr_t page = (intptr_t)ROUND_PG_DOWN((uintptr_t)addr);
  size_t span = (size_t)(ROUND_PG_UP((uintptr_t)addr + len) -
                         (uintptr_t)page);

  kernel_mprotect(pid, page, span, PROT_READ | PROT_WRITE | PROT_EXEC);
  if(pt_copyin(pid, data, addr, len) < 0) {
    kernel_mprotect(pid, page, span, PROT_READ | PROT_EXEC);
    return -1;
  }

  uint8_t verify[256];
  size_t off = 0;
  int mismatch = 0;
  while(off < len) {
    size_t chunk = len - off;
    if(chunk > sizeof(verify)) chunk = sizeof(verify);
    if(pt_copyout(pid, addr + (intptr_t)off, verify, chunk) < 0) {
      /* restore PROT_RX so anti-cheat scans see the original layout
         even on read-failure path */
      kernel_mprotect(pid, page, span, PROT_READ | PROT_EXEC);
      return -2;
    }
    if(memcmp(verify, data + off, chunk) != 0) { mismatch = 1; break; }
    off += chunk;
  }
  kernel_mprotect(pid, page, span, PROT_READ | PROT_EXEC);
  return mismatch ? -3 : 0;
}

static int
resolve_module_base(pid_t pid, const char *module_name,
                    intptr_t eboot_base, intptr_t *out_base) {
  if(!module_name || !*module_name) {
    *out_base = eboot_base;
    return 0;
  }
  uint32_t handle = 0;
  if(kernel_dynlib_handle(pid, module_name, &handle) != 0) return -1;
  intptr_t b = kernel_dynlib_mapbase_addr(pid, handle);
  if(!b) return -1;
  *out_base = b;
  return 0;
}

static int
process_is_ps2_emu(pid_t pid) {
  uint32_t handle = 0;
  return kernel_dynlib_handle(pid, "libScePs2EmuMenuDialog.sprx",
                              &handle) == 0;
}

static int
write_via_codecave(pid_t pid, intptr_t addr, const uint8_t *data,
                   size_t len) {
  intptr_t page = (intptr_t)ROUND_PG_DOWN((uintptr_t)addr);
  size_t span = (size_t)(ROUND_PG_UP((uintptr_t)addr + len) -
                         (uintptr_t)page);
  intptr_t mem = pt_mmap(pid, page, span, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if(mem == -1 || mem == 0) return -1;
  if(pt_copyin(pid, data, addr, len) < 0) return -1;
  kernel_mprotect(pid, mem, span, PROT_READ | PROT_EXEC | PROT_WRITE);
  kernel_mprotect(pid, mem, span, PROT_READ | PROT_EXEC);
  return 0;
}


static cJSON*
find_master_code_mod(cJSON *mods) {
  if(!cJSON_IsArray(mods)) return NULL;
  cJSON *m;
  cJSON_ArrayForEach(m, mods) {
    cJSON *nm = cJSON_GetObjectItem(m, "name");
    if(!cJSON_IsString(nm) || !nm->valuestring) continue;
    if(strstr(nm->valuestring, CHEAT_MC_NAME_TOK_A) ||
       strstr(nm->valuestring, CHEAT_MC_NAME_TOK_B)) {
      return m;
    }
  }
  return NULL;
}

static int
mod_is_mc_dependent(const char *name) {
  if(!name || !*name) return 0;
  if(strstr(name, CHEAT_MC_NAME_TOK_A) ||
     strstr(name, CHEAT_MC_NAME_TOK_B)) return 0;
  return strstr(name, CHEAT_MC_DEP_NAME_TOK) ? 1 : 0;
}

static int
fixup_mc_dependent_addr(pid_t pid, intptr_t mc_addr,
                        const uint8_t *mc_on, size_t mc_on_len,
                        const uint8_t *dep_off, size_t dep_off_len,
                        uint64_t mc_offset_raw, uint64_t dep_offset_raw,
                        intptr_t *addr) {
  if(!mc_on_len || !dep_off_len || dep_off_len > mc_on_len) return 0;
  uint8_t *buf = (uint8_t*)malloc(mc_on_len);
  if(!buf) return 0;
  if(pt_copyout(pid, mc_addr, buf, mc_on_len) < 0) {
    free(buf);
    return 0;
  }
  if(memcmp(buf, mc_on, mc_on_len) != 0) {
  }
  for(size_t i = 0; i + dep_off_len <= mc_on_len; i++) {
    if(memcmp(buf + i, dep_off, dep_off_len) == 0) {
      *addr = mc_addr + (intptr_t)i;
      free(buf);
      return 1;
    }
  }
  free(buf);
  *addr = (intptr_t)((mc_offset_raw & ~(uint64_t)0xff) |
                     (dep_offset_raw & 0xff));
  return 1;
}


static int
apply_cheat(const char *title_id, int mod_index, int turn_on,
            char *err, size_t err_size) {
  pid_t pid = -1;
  intptr_t base = 0;
  char running_title[16];
  char path[256];

  if(!cheats_engine_enabled()) {
    snprintf(err, err_size, "cheat engine is disabled");
    return -1;
  }
  if(get_running_game(&pid, running_title, sizeof(running_title), &base) != 0) {
    snprintf(err, err_size, "no game is currently running");
    return -1;
  }
  {
    char run_norm[10], cheat_norm[10];
    if(!title_id_normalize(running_title, run_norm) ||
       !title_id_normalize(title_id, cheat_norm) ||
       strcmp(run_norm, cheat_norm) != 0) {
      snprintf(err, err_size,
               "running game (%s) does not match cheat target (%s)",
               running_title, title_id);
      return -1;
    }
  }
  int kind = 0;
  if(!find_cheat_file_for_title(title_id, path, sizeof(path), &kind)) {
    snprintf(err, err_size,
             "no cheat file for %s. Drop a %s.json/.shn/.mc4 (or "
             "%s_<version>.json/.shn/.mc4) into %s via FTP.",
             title_id, title_id, title_id, CHEATS_DIR);
    return -1;
  }

  size_t len = 0;
  char *txt = read_file_text(path, &len);
  if(!txt) {
    snprintf(err, err_size, "could not read %s", path);
    return -1;
  }

  /* SHN is XML; MC4 is base64+AES-256-CBC of the same XML schema. Both
     get converted to the JSON shape apply_cheat() expects. */
  char *converted = NULL;
  if(kind == 2) { /* SHN */
    converted = shn_xml_to_json(txt, len);
    if(!converted) {
      free(txt);
      snprintf(err, err_size, "SHN parse failed for %s", path);
      return -1;
    }
  } else if(kind == 3) { /* MC4 */
    char *xml = mc4_decrypt_to_xml(txt, len, NULL);
    if(!xml) {
      free(txt);
      snprintf(err, err_size,
               "MC4 decrypt failed for %s — file may be corrupt", path);
      return -1;
    }
    converted = shn_xml_to_json(xml, strlen(xml));
    free(xml);
    if(!converted) {
      free(txt);
      snprintf(err, err_size, "MC4 XML parse failed for %s", path);
      return -1;
    }
  }

  cJSON *root = cJSON_Parse(converted ? converted : txt);
  free(converted);
  free(txt);
  if(!root) {
    snprintf(err, err_size, "cheat parse failed for %s", path);
    return -1;
  }

  cJSON *mods = cJSON_GetObjectItem(root, "mods");
  if(!cJSON_IsArray(mods) ||
     mod_index < 0 ||
     mod_index >= cJSON_GetArraySize(mods)) {
    snprintf(err, err_size, "cheat index out of range");
    cJSON_Delete(root);
    return -1;
  }
  cJSON *mod = cJSON_GetArrayItem(mods, mod_index);
  cJSON *type_j = cJSON_GetObjectItem(mod, "type");
  const char *type = (cJSON_IsString(type_j) && type_j->valuestring)
                       ? type_j->valuestring : "checkbox";
  cJSON *memory = cJSON_GetObjectItem(mod, "memory");
  if(!cJSON_IsArray(memory) || cJSON_GetArraySize(memory) == 0) {
    snprintf(err, err_size, "mod has no memory entries");
    cJSON_Delete(root);
    return -1;
  }

  cJSON *mod_name_j = cJSON_GetObjectItem(mod, "module_name");
  const char *mod_name = (cJSON_IsString(mod_name_j) && mod_name_j->valuestring)
                            ? mod_name_j->valuestring : "";
  intptr_t mod_base = 0;
  if(resolve_module_base(pid, mod_name, base, &mod_base) != 0) {
    snprintf(err, err_size,
             "module \"%s\" is not loaded in the target process",
             mod_name);
    cJSON_Delete(root);
    return -1;
  }

  int is_ps2 = process_is_ps2_emu(pid);

  /* "button" mods are one-shot writes — always apply the "on" payload
     regardless of toggle state. */
  int effective_on = (!strcasecmp(type, "button")) ? 1 : (turn_on ? 1 : 0);

  cJSON     *mc_mod         = NULL;
  intptr_t   mc_addr        = 0;
  uint64_t   mc_offset_raw  = 0;
  uint8_t   *mc_on_bytes    = NULL;
  size_t     mc_on_len      = 0;
  int        do_mc_fixup    = 0;
  {
    cJSON *nm_j = cJSON_GetObjectItem(mod, "name");
    const char *nm = (cJSON_IsString(nm_j) && nm_j->valuestring)
                       ? nm_j->valuestring : "";
    if(mod_is_mc_dependent(nm)) {
      mc_mod = find_master_code_mod(mods);
      if(mc_mod) {
        cJSON *mc_mem = cJSON_GetObjectItem(mc_mod, "memory");
        cJSON *mc0    = (cJSON_IsArray(mc_mem) &&
                          cJSON_GetArraySize(mc_mem) > 0)
                          ? cJSON_GetArrayItem(mc_mem, 0) : NULL;
        cJSON *mc_off_j = mc0 ? cJSON_GetObjectItem(mc0, "offset") : NULL;
        cJSON *mc_on_j  = mc0 ? cJSON_GetObjectItem(mc0, "on")     : NULL;
        cJSON *mc_abs_j = mc0 ? cJSON_GetObjectItem(mc0, "absolute") : NULL;
        if(cJSON_IsString(mc_off_j) && cJSON_IsString(mc_on_j) &&
           parse_hex_bytes(mc_on_j->valuestring, &mc_on_bytes, &mc_on_len) == 0
           && mc_on_len > 0) {
          mc_offset_raw = parse_offset(mc_off_j->valuestring);
          int mc_abs = (cJSON_IsTrue(mc_abs_j) || is_ps2) ? 1 : 0;
          mc_addr = mc_abs ? (intptr_t)mc_offset_raw
                           : (mod_base + (intptr_t)mc_offset_raw);
          do_mc_fixup = 1;
        }
      }
    }
  }

  if(pt_attach(pid) < 0) {
    snprintf(err, err_size, "pt_attach failed (errno=%d)", errno);
    cJSON_Delete(root);
    return -1;
  }

  int rc = 0;
  cJSON *m;
  cJSON_ArrayForEach(m, memory) {
    cJSON *off_j  = cJSON_GetObjectItem(m, "offset");
    cJSON *on_j   = cJSON_GetObjectItem(m, "on");
    cJSON *off2_j = cJSON_GetObjectItem(m, "off");
    cJSON *abs_j  = cJSON_GetObjectItem(m, "absolute");
    if(!cJSON_IsString(off_j) || !cJSON_IsString(on_j) ||
       !cJSON_IsString(off2_j)) {
      snprintf(err, err_size, "memory entry missing offset/on/off");
      rc = -1;
      break;
    }
    uint64_t off = parse_offset(off_j->valuestring);
    int absolute = (cJSON_IsTrue(abs_j) || is_ps2) ? 1 : 0;

    uint8_t *on_bytes = NULL, *off_bytes = NULL;
    size_t on_len = 0, off_len = 0;
    if(parse_hex_bytes(on_j->valuestring,  &on_bytes,  &on_len) != 0 ||
       parse_hex_bytes(off2_j->valuestring, &off_bytes, &off_len) != 0) {
      snprintf(err, err_size, "memory hex parse failed");
      free(on_bytes); free(off_bytes);
      rc = -1;
      break;
    }

    intptr_t addr = absolute ? (intptr_t)off : (mod_base + (intptr_t)off);
    const uint8_t *data = effective_on ? on_bytes : off_bytes;
    size_t        wlen  = effective_on ? on_len   : off_len;

    if(do_mc_fixup && !absolute && off_len > 0) {
      fixup_mc_dependent_addr(pid, mc_addr, mc_on_bytes, mc_on_len,
                              off_bytes, off_len, mc_offset_raw, off,
                              &addr);
    }

    if(wlen == 0 || !data) {
      free(on_bytes); free(off_bytes);
      continue;
    }

    {
      int wrc = write_process_memory(pid, addr, data, wlen);
      if(wrc == -3) {
        if(!absolute &&
           write_via_codecave(pid, addr, data, wlen) == 0) {
          /* code cave success — patch is live at the address */
        } else {
          snprintf(err, err_size,
                   "patch did not stick at 0x%lx (len %zu) — wrong "
                   "cheat for this build of the game, or page is "
                   "anti-cheat sealed and cave reloc failed (try a "
                   "fresh download / matching eboot patch level, or "
                   "restart the game before re-applying)",
                   (long)addr, wlen);
          free(on_bytes); free(off_bytes);
          rc = -1;
          break;
        }
      } else if(wrc != 0) {
        const char *what =
          (wrc == -1) ? "kernel rejected the write"
        :               "could not read back the patched bytes";
        snprintf(err, err_size, "%s at 0x%lx (len %zu)",
                 what, (long)addr, wlen);
        free(on_bytes); free(off_bytes);
        rc = -1;
        break;
      }
    }

    free(on_bytes);
    free(off_bytes);
  }

  if(rc == 0 && strcasecmp(type, "button") != 0 && kind == 1) {
    cJSON_DeleteItemFromObject(mod, "_sonic_enabled");
    cJSON_AddBoolToObject(mod, "_sonic_enabled", turn_on ? 1 : 0);
    char *out = cJSON_Print(root);
    if(out) {
      write_file_bytes(path, out, strlen(out));
      free(out);
    }
  }

  pt_detach(pid, 0);
  free(mc_on_bytes);
  cJSON_Delete(root);
  return rc;
}


/*  HTTP handlers                                                         */

static int
local_json_extract_string(const char *json, const char *key,
                          char *out, size_t out_size) {
  if(!json || !key || !out || out_size < 2) return 0;
  char needle[64];
  int wn = snprintf(needle, sizeof(needle), "\"%s\"", key);
  if(wn <= 0 || (size_t)wn >= sizeof(needle)) return 0;
  const char *p = strstr(json, needle);
  if(!p) return 0;
  p += strlen(needle);
  while(*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
  if(*p != ':') return 0;
  p++;
  while(*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
  if(*p != '"') return 0;
  p++;
  size_t i = 0;
  while(*p && *p != '"' && i < out_size - 1) {
    if(*p == '\\' && p[1]) { out[i++] = p[1]; p += 2; }
    else                   { out[i++] = *p++;            }
  }
  out[i] = 0;
  return i > 0 ? 1 : 0;
}


static void
read_cheat_display_name(const char *path, int kind,
                        char *out, size_t out_size) {
  out[0] = 0;

  if(kind == 3) {
    /* MC4 — must decrypt the whole file before scanning for the name. */
    size_t len = 0;
    char *raw = read_file_text(path, &len);
    if(!raw) return;
    char *xml = mc4_decrypt_to_xml(raw, len, NULL);
    free(raw);
    if(!xml) return;
    const char *t = strstr(xml, "<Trainer");
    if(t) {
      const char *end = strchr(t, '>');
      if(end) {
        char tag[1024];
        size_t tn = (size_t)(end - t);
        if(tn < sizeof(tag)) {
          memcpy(tag, t, tn); tag[tn] = 0;
          size_t alen = 0;
          const char *v = xml_find_attr(tag, "Game", &alen);
          if(!v) v = xml_find_attr(tag, "GameName", &alen);
          if(v && alen > 0 && alen < out_size) {
            memcpy(out, v, alen);
            out[alen] = 0;
          }
        }
      }
    }
    free(xml);
    return;
  }

  int fd = open(path, O_RDONLY);
  if(fd < 0) return;
  char head[4096];
  ssize_t n = read(fd, head, sizeof(head)-1);
  close(fd);
  if(n <= 0) return;
  head[n] = 0;

  if(kind == 1) {
    /* JSON. */
    local_json_extract_string(head, "name", out, out_size);
  } else if(kind == 2) {
    /* SHN — pull the Trainer attribute. */
    const char *t = strstr(head, "<Trainer");
    if(t) {
      const char *end = strchr(t, '>');
      if(end) {
        char tag[1024];
        size_t tn = (size_t)(end - t);
        if(tn < sizeof(tag)) {
          memcpy(tag, t, tn); tag[tn] = 0;
          size_t alen = 0;
          const char *v = xml_find_attr(tag, "Game", &alen);
          if(!v) v = xml_find_attr(tag, "GameName", &alen);
          if(v && alen > 0 && alen < out_size) {
            memcpy(out, v, alen);
            out[alen] = 0;
          }
        }
      }
    }
  }
}


static enum MHD_Result
list_cheats(struct MHD_Connection *conn) {
  ensure_cheats_dir();

  cJSON *root = cJSON_CreateObject();
  cJSON *files = cJSON_AddArrayToObject(root, "files");

  /* Scan flat root + all three format subdirs to find every title that has
     a cheat file.  download-all writes only into the subdirs, so scanning
     only the flat root (old behaviour) would always return an empty list. */
  static const char * const scan_dirs[] = {
    CHEATS_DIR, CHEATS_SUBDIR_JSON, CHEATS_SUBDIR_SHN, CHEATS_SUBDIR_MC4
  };
  char seen[512][16];
  int  seen_n = 0;
  for(int di = 0; di < (int)(sizeof(scan_dirs)/sizeof(scan_dirs[0])); di++) {
    DIR *d = opendir(scan_dirs[di]);
    if(!d) continue;
    struct dirent *ent;
    while((ent = readdir(d))) {
      const char *name = ent->d_name;
      if(name[0] == '.') continue;
      if(!recognised_cheat_extension(name)) continue;
      char id[16];
      if(!extract_title_id_prefix(name, id, sizeof(id))) continue;
      int dup = 0;
      for(int i=0; i<seen_n; i++) {
        if(!strcmp(seen[i], id)) { dup = 1; break; }
      }
      if(dup) continue;
      if(seen_n < (int)(sizeof(seen)/sizeof(seen[0]))) {
        strncpy(seen[seen_n], id, 16);
        seen[seen_n][15] = 0;
        seen_n++;
      }

      char path[256];
      int  kind = 0;
      char display[256];
      display[0] = 0;
      if(find_cheat_file_for_title(id, path, sizeof(path), &kind)) {
        read_cheat_display_name(path, kind, display, sizeof(display));
      }

      cJSON *e = cJSON_CreateObject();
      cJSON_AddStringToObject(e, "titleId", id);
      cJSON_AddStringToObject(e, "name",    display[0] ? display : id);
      cJSON_AddItemToArray(files, e);
    }
    closedir(d);
  }

  pid_t pid = -1;
  intptr_t base = 0;
  char running[16] = {0};
  if(get_running_game(&pid, running, sizeof(running), &base) == 0) {
    cJSON *running_obj = cJSON_AddObjectToObject(root, "running");
    cJSON_AddStringToObject(running_obj, "titleId", running);
    cJSON_AddNumberToObject(running_obj, "pid", pid);
    char base_hex[32];
    snprintf(base_hex, sizeof(base_hex), "0x%lx", (long)base);
    cJSON_AddStringToObject(running_obj, "imageBase", base_hex);
  } else {
    cJSON_AddNullToObject(root, "running");
  }

  enum MHD_Result ret = serve_json_object(conn, MHD_HTTP_OK, root);
  cJSON_Delete(root);
  return ret;
}


/* Find a specific format kind (1=json,2=shn,3=mc4) in the flat root
   and the matching format subdir.  Returns 1 and fills out_path on success. */
static int
find_format_file(const char *title_id, size_t id_len, int target_kind,
                 char *out_path, size_t out_size) {
  const char *dirs[2] = {
    CHEATS_DIR,
    target_kind == 1 ? CHEATS_SUBDIR_JSON
  : target_kind == 2 ? CHEATS_SUBDIR_SHN
  :                    CHEATS_SUBDIR_MC4,
  };
  for(int i = 0; i < 2; i++) {
    int bk = 0;
    char bp[MAX_CHEAT_FILEPATH_LEN] = {0};
    scan_one_cheats_dir(dirs[i], title_id, id_len, &bk, bp, sizeof(bp));
    if(bk == target_kind) {
      if(out_path) {
        size_t l = strlen(bp);
        if(l >= out_size) return 0;
        memcpy(out_path, bp, l + 1);
      }
      return 1;
    }
  }
  return 0;
}

/* Fill kinds[]/paths[] with every available format for title_id, in priority
   order (JSON first, SHN, MC4).  Returns the count (0–3). */
static int
find_all_formats(const char *title_id,
                 int kinds[3], char paths[3][MAX_CHEAT_FILEPATH_LEN]) {
  size_t id_len = strlen(title_id);
  int n = 0;
  for(int k = 1; k <= 3; k++) {
    char p[MAX_CHEAT_FILEPATH_LEN];
    if(find_format_file(title_id, id_len, k, p, sizeof(p))) {
      if(kinds) kinds[n] = k;
      if(paths) memcpy(paths[n], p, strlen(p) + 1);
      n++;
    }
  }
  return n;
}

static enum MHD_Result
get_cheats_for(struct MHD_Connection *conn, const char *title_id) {
  static const char * const kname[] = {NULL, "json", "shn", "mc4"};

  if(!is_safe_title_id(title_id))
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "bad titleId");

  /* Optional ?format=json|shn|mc4 — lets the UI pick a specific file. */
  const char *fmt_s = MHD_lookup_connection_value(
    conn, MHD_GET_ARGUMENT_KIND, "format");
  int req_kind = 0;
  if(fmt_s) {
    if(!strcasecmp(fmt_s, "json"))     req_kind = 1;
    else if(!strcasecmp(fmt_s, "shn")) req_kind = 2;
    else if(!strcasecmp(fmt_s, "mc4")) req_kind = 3;
  }

  int  kinds[3] = {0};
  char paths[3][MAX_CHEAT_FILEPATH_LEN];
  int  n = find_all_formats(title_id, kinds, paths);
  if(n == 0)
    return serve_error(conn, MHD_HTTP_NOT_FOUND, "no cheat file");

  /* Pick file: requested kind if available, otherwise best priority (idx 0). */
  int chosen = 0;
  if(req_kind) {
    for(int i = 0; i < n; i++)
      if(kinds[i] == req_kind) { chosen = i; break; }
  }

  int kind = kinds[chosen];
  size_t len = 0;
  char *txt = read_file_text(paths[chosen], &len);
  if(!txt)
    return serve_error(conn, MHD_HTTP_NOT_FOUND, "could not read cheat file");

  /* Convert to JSON string. */
  char *json = NULL;
  if(kind == 1) {
    json = txt; txt = NULL;
  } else if(kind == 2) {
    json = shn_xml_to_json(txt, len);
    free(txt); txt = NULL;
    if(!json) return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "SHN parse failed");
  } else {
    char *xml = mc4_decrypt_to_xml(txt, len, NULL);
    free(txt); txt = NULL;
    if(!xml) return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "MC4 decrypt failed");
    json = shn_xml_to_json(xml, strlen(xml));
    free(xml);
    if(!json) return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "MC4 XML parse failed");
  }

  /* Build "formats" JSON array: e.g. ["shn","mc4"] */
  char fmts_json[64];
  int fp = 0;
  fmts_json[fp++] = '[';
  for(int i = 0; i < n; i++) {
    if(i) fmts_json[fp++] = ',';
    fmts_json[fp++] = '"';
    const char *kn = kname[kinds[i]];
    size_t kl = strlen(kn);
    memcpy(fmts_json + fp, kn, kl); fp += (int)kl;
    fmts_json[fp++] = '"';
  }
  fmts_json[fp++] = ']'; fmts_json[fp] = 0;

  /* Inject formats + activeFormat before the final closing '}'. */
  char inject[128];
  int inj = snprintf(inject, sizeof(inject),
    ",\"formats\":%s,\"activeFormat\":\"%s\"}",
    fmts_json, kname[kind]);

  char *end = strrchr(json, '}');
  if(!end || inj <= 0) {
    free(json);
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "bad json");
  }
  *end = '\0';
  size_t base = (size_t)(end - json);
  char *out = malloc(base + (size_t)inj + 1);
  if(!out) { free(json); return MHD_NO; }
  memcpy(out, json, base);
  memcpy(out + base, inject, (size_t)inj + 1);
  free(json);
  return serve_buffer(conn, MHD_HTTP_OK, "application/json",
                      out, base + (size_t)inj, 1);
}


static enum MHD_Result
delete_cheats_for(struct MHD_Connection *conn, const char *title_id) {
  if(!is_safe_title_id(title_id)) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "bad titleId");
  }
  char path[256];
  int kind = 0;
  if(find_cheat_file_for_title(title_id, path, sizeof(path), &kind)) {
    if(unlink(path) != 0 && errno != ENOENT) {
      return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, strerror(errno));
    }
  }
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", 1);
  enum MHD_Result ret = serve_json_object(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


static enum MHD_Result
toggle_cheat(struct MHD_Connection *conn) {
  const char *title_id = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "titleId");
  const char *idx_s    = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "index");
  const char *on_s     = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "on");

  if(!title_id || !idx_s || !on_s) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "missing args");
  }
  int idx = atoi(idx_s);
  int on  = (strcmp(on_s, "0") != 0);

  char err[256] = {0};
  int rc = apply_cheat(title_id, idx, on, err, sizeof(err));
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", rc == 0);
  if(rc != 0) {
    cJSON_AddStringToObject(r, "error", err[0] ? err : "apply failed");
  } else {
    cJSON_AddBoolToObject(r, "enabled", on ? 1 : 0);
  }
  enum MHD_Result ret = serve_json_object(conn,
                                          rc == 0 ? MHD_HTTP_OK
                                                  : MHD_HTTP_BAD_REQUEST, r);
  cJSON_Delete(r);
  return ret;
}


/*  Auto-download from GoldHEN_Cheat_Repository / "etaHEN PS5_Cheats"     */

struct cheat_repo_fmt {
  const char *name;     /* "json" | "shn" | "mc4" */
  const char *index;    /* full URL to the .txt index */
  const char *base;     /* directory base URL (with trailing slash) */
};

static const struct cheat_repo_fmt PS5_CHEATS_FORMATS[] = {
  {"json", "https://raw.githubusercontent.com/etaHEN/PS5_Cheats/main/json.txt",
           "https://raw.githubusercontent.com/etaHEN/PS5_Cheats/main/json/"},
  {"shn",  "https://raw.githubusercontent.com/etaHEN/PS5_Cheats/main/shn.txt",
           "https://raw.githubusercontent.com/etaHEN/PS5_Cheats/main/shn/"},
  {"mc4",  "https://raw.githubusercontent.com/etaHEN/PS5_Cheats/main/mc4.txt",
           "https://raw.githubusercontent.com/etaHEN/PS5_Cheats/main/mc4/"},
};

static const struct cheat_repo_fmt GOLDHEN_FORMATS[] = {
  {"json", "https://raw.githubusercontent.com/GoldHEN/GoldHEN_Cheat_Repository/main/json.txt",
           "https://raw.githubusercontent.com/GoldHEN/GoldHEN_Cheat_Repository/main/json/"},
  {"shn",  "https://raw.githubusercontent.com/GoldHEN/GoldHEN_Cheat_Repository/main/shn.txt",
           "https://raw.githubusercontent.com/GoldHEN/GoldHEN_Cheat_Repository/main/shn/"},
  {"mc4",  "https://raw.githubusercontent.com/GoldHEN/GoldHEN_Cheat_Repository/main/mc4.txt",
           "https://raw.githubusercontent.com/GoldHEN/GoldHEN_Cheat_Repository/main/mc4/"},
};

static const struct cheat_repo_fmt HEN_COLLECTION_FORMATS[] = {
  {"json", "https://raw.githubusercontent.com/TeeKay87/HEN-Cheats-Collection/master/cheats/json.txt",
           "https://raw.githubusercontent.com/TeeKay87/HEN-Cheats-Collection/master/cheats/json/"},
  {"shn",  "https://raw.githubusercontent.com/TeeKay87/HEN-Cheats-Collection/master/cheats/shn.txt",
           "https://raw.githubusercontent.com/TeeKay87/HEN-Cheats-Collection/master/cheats/shn/"},
  {"mc4",  "https://raw.githubusercontent.com/TeeKay87/HEN-Cheats-Collection/master/cheats/mc4.txt",
           "https://raw.githubusercontent.com/TeeKay87/HEN-Cheats-Collection/master/cheats/mc4/"},
};

#define CHEAT_FORMATS_PER_REPO ((int)(sizeof(PS5_CHEATS_FORMATS)/sizeof(PS5_CHEATS_FORMATS[0])))

static const struct cheat_repo_fmt *
cheat_formats_for(const char *source, int *count_out) {
  *count_out = CHEAT_FORMATS_PER_REPO;
  if(source && !strcasecmp(source, "goldhen"))    return GOLDHEN_FORMATS;
  if(source && !strcasecmp(source, "hencollection")) return HEN_COLLECTION_FORMATS;
  return PS5_CHEATS_FORMATS;
}


typedef struct {
  const char *source;        /* matches sourceCheatSel.value in the UI */
  const char *zip_url;       /* codeload direct download */
  const char *entry_prefix;  /* "<repo-name>-<branch>/" prefix on every entry */
  const char *cheats_subdir; /* "" for repos that store at root, "cheats/" for HEN-Collection */
} cheat_zip_repo_t;

static const cheat_zip_repo_t CHEAT_ZIP_REPOS[] = {
  {"ps5cheats",
   "https://codeload.github.com/etaHEN/PS5_Cheats/zip/refs/heads/main",
   "PS5_Cheats-main/", ""},
  {"goldhen",
   "https://codeload.github.com/GoldHEN/GoldHEN_Cheat_Repository/zip/refs/heads/main",
   "GoldHEN_Cheat_Repository-main/", ""},
  {"hencollection",
   "https://codeload.github.com/TeeKay87/HEN-Cheats-Collection/zip/refs/heads/master",
   "HEN-Cheats-Collection-master/", "cheats/"},
};
#define CHEAT_ZIP_REPOS_N ((int)(sizeof(CHEAT_ZIP_REPOS)/sizeof(CHEAT_ZIP_REPOS[0])))

static const cheat_zip_repo_t *
cheat_zip_for(const char *source) {
  if(!source) source = "ps5cheats";
  for(int i = 0; i < CHEAT_ZIP_REPOS_N; i++) {
    if(!strcasecmp(source, CHEAT_ZIP_REPOS[i].source))
      return &CHEAT_ZIP_REPOS[i];
  }
  return &CHEAT_ZIP_REPOS[0];
}


/* Find an entry in `json.txt` whose filename starts with `<title_id>_`.
   The first match wins. Returns a malloc'd filename on success. */
static char*
find_index_match(const char *index_text, const char *title_id) {
  size_t idlen = strlen(title_id);
  const char *p = index_text;
  while(p && *p) {
    const char *nl = strchr(p, '\n');
    size_t llen = nl ? (size_t)(nl - p) : strlen(p);
    if(llen > idlen + 1 &&
       !strncmp(p, title_id, idlen) &&
       p[idlen] == '_') {
      const char *eq = memchr(p, '=', llen);
      size_t flen = eq ? (size_t)(eq - p) : llen;
      char *out = malloc(flen + 1);
      memcpy(out, p, flen);
      out[flen] = 0;
      return out;
    }
    if(!nl) break;
    p = nl + 1;
  }
  return NULL;
}


static enum MHD_Result
download_cheats(struct MHD_Connection *conn) {
  const char *title_id = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "titleId");
  const char *source   = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "source");
  if(!title_id || !is_safe_title_id(title_id)) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "bad titleId");
  }
  if(!source) source = "ps5cheats";

  int n_fmts = 0;
  const struct cheat_repo_fmt *fmts = cheat_formats_for(source, &n_fmts);

  char *match = NULL;
  const struct cheat_repo_fmt *hit = NULL;
  for(int i = 0; i < n_fmts && !match; i++) {
    size_t ilen = 0;
    uint8_t *index = http_get(fmts[i].index, &ilen);
    if(!index || ilen == 0) { free(index); continue; }
    uint8_t *idx_z = realloc(index, ilen + 1);
    if(!idx_z) { free(index); continue; }
    idx_z[ilen] = 0;
    match = find_index_match((const char*)idx_z, title_id);
    free(idx_z);
    if(match) hit = &fmts[i];
  }
  if(!match || !hit) {
    return serve_error(conn, MHD_HTTP_NOT_FOUND,
        "no upstream cheat for that title (checked json/shn/mc4)");
  }

  char url[512];
  snprintf(url, sizeof(url), "%s%s", hit->base, match);
  size_t flen = 0;
  uint8_t *body = http_get(url, &flen);
  if(!body || flen == 0) {
    free(match);
    return serve_error(conn, MHD_HTTP_BAD_GATEWAY, "could not fetch cheat file");
  }

  ensure_cheats_dir();
  char path[256];
  if(!strcmp(hit->name, "json")) {
    snprintf(path, sizeof(path), "%s/%s.json", CHEATS_DIR, title_id);
  } else {
    snprintf(path, sizeof(path), "%s/%s", CHEATS_DIR, match);
  }
  int wrc = write_file_bytes(path, body, flen);
  free(body);
  if(wrc != 0) {
    free(match);
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "write failed");
  }

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", 1);
  cJSON_AddStringToObject(r, "titleId", title_id);
  cJSON_AddStringToObject(r, "source",  source);
  cJSON_AddStringToObject(r, "format",  hit->name);
  cJSON_AddStringToObject(r, "upstream", match);
  cJSON_AddStringToObject(r, "path",    path);
  cJSON_AddNumberToObject(r, "size", (double)flen);
  enum MHD_Result ret = serve_json_object(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  free(match);
  return ret;
}



#define DL_STATE_IDLE     0
#define DL_STATE_RUNNING  1
#define DL_STATE_DONE     2
#define DL_STATE_ERROR    3

#define DL_MAX_MISSING    20

typedef struct {
  pthread_mutex_t lock;
  int    state;             /* DL_STATE_* */
  char   source[16];        /* "ps5cheats" / "goldhen" */
  int    total;             /* indexed entries */
  int    downloaded;
  int    skipped;
  int    failed;
  int    verified;          /* 1 once verification ran */
  int    missing_count;     /* # of expected files not on disk afterwards */
  char   missing[DL_MAX_MISSING][96];
  char   current[128];      /* filename currently being fetched */
  char   error[256];        /* set on DL_STATE_ERROR */
  time_t started_at;
  time_t finished_at;
} dl_progress_t;

static dl_progress_t g_dl = {
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .state = DL_STATE_IDLE,
};


static void
dl_set_state_locked(int new_state) {
  g_dl.state = new_state;
  if(new_state == DL_STATE_DONE || new_state == DL_STATE_ERROR) {
    g_dl.finished_at = time(NULL);
    g_dl.current[0] = 0;
  }
}


static int
process_one_repo_zip(const cheat_zip_repo_t *repo,
                     int *out_added, int *out_skipped, int *out_failed,
                     int *out_planned,
                     char (*expected)[96], size_t expected_cap,
                     size_t *expected_n_io) {
  size_t prefix_len        = strlen(repo->entry_prefix);
  size_t cheats_subdir_len = strlen(repo->cheats_subdir);
  int    planned_total     = 0;

  pthread_mutex_lock(&g_dl.lock);
  snprintf(g_dl.current, sizeof(g_dl.current),
           "Downloading %s zip…", repo->source);
  pthread_mutex_unlock(&g_dl.lock);

  size_t zlen = 0;
  uint8_t *zip = http_get(repo->zip_url, &zlen);
  if(!zip || zlen == 0) {
    free(zip);
    pthread_mutex_lock(&g_dl.lock);
    snprintf(g_dl.current, sizeof(g_dl.current),
             "%s skipped (network / TLS / rate-limit)", repo->source);
    pthread_mutex_unlock(&g_dl.lock);
    return -1;
  }

  pthread_mutex_lock(&g_dl.lock);
  snprintf(g_dl.current, sizeof(g_dl.current),
           "Extracting %s (%.1f MB)…", repo->source,
           zlen / (1024.0 * 1024.0));
  pthread_mutex_unlock(&g_dl.lock);

  /* Pass 1 — count matching entries. */
  {
    ezip *a = ezip_open_mem(zip, zlen);
    if(!ezip_failed(a)) {
      ezip_entry entry;
      while(ezip_next(a, &entry) == 1) {
        const char *name = entry.path;
        if(!name || entry.is_dir) continue;
        if(strncmp(name, repo->entry_prefix, prefix_len) != 0) continue;
        const char *rel = name + prefix_len;
        if(cheats_subdir_len &&
           strncmp(rel, repo->cheats_subdir, cheats_subdir_len) == 0)
          rel += cheats_subdir_len;
        if(!strncmp(rel, "json/", 5) ||
           !strncmp(rel, "shn/",  4) ||
           !strncmp(rel, "mc4/",  4))
          planned_total++;
      }
    }
    ezip_close(a);
  }
  if(out_planned) *out_planned += planned_total;

  pthread_mutex_lock(&g_dl.lock);
  g_dl.total = out_planned ? *out_planned : planned_total;
  pthread_mutex_unlock(&g_dl.lock);

  if(planned_total == 0) { free(zip); return 0; }

  /* Pass 2 — extract. */
  ezip *a = ezip_open_mem(zip, zlen);
  if(ezip_failed(a)) {
    ezip_close(a); free(zip); return -1;
  }

  ezip_entry entry;
  while(ezip_next(a, &entry) == 1) {
    const char *name = entry.path;
    if(!name || entry.is_dir) continue;
    if(strncmp(name, repo->entry_prefix, prefix_len) != 0) continue;
    const char *rel = name + prefix_len;
    if(cheats_subdir_len &&
       strncmp(rel, repo->cheats_subdir, cheats_subdir_len) == 0)
      rel += cheats_subdir_len;

    const char *subdir = NULL;
    if(!strncmp(rel, "json/", 5))      { subdir = CHEATS_SUBDIR_JSON; rel += 5; }
    else if(!strncmp(rel, "shn/",  4)) { subdir = CHEATS_SUBDIR_SHN;  rel += 4; }
    else if(!strncmp(rel, "mc4/",  4)) { subdir = CHEATS_SUBDIR_MC4;  rel += 4; }
    else continue;
    if(strstr(rel, "..") || strchr(rel, '/')) continue;

    char dest[MAX_CHEAT_FILEPATH_LEN];
    if(snprintf(dest, sizeof(dest), "%s/%s", subdir, rel) >= (int)sizeof(dest)) {
      if(out_failed) (*out_failed)++; goto tick;
    }
    if(*expected_n_io < expected_cap) {
      strncpy(expected[*expected_n_io], rel,
              sizeof(expected[*expected_n_io])-1);
      expected[*expected_n_io][sizeof(expected[*expected_n_io])-1] = 0;
      (*expected_n_io)++;
    }
    pthread_mutex_lock(&g_dl.lock);
    strncpy(g_dl.current, rel, sizeof(g_dl.current)-1);
    g_dl.current[sizeof(g_dl.current)-1] = 0;
    pthread_mutex_unlock(&g_dl.lock);

    /* Skip-existing — load-bearing for the "verify with another repo"
       UX (etaHEN then GoldHEN then TeeKay87 fills the union). */
    struct stat st;
    if(stat(dest, &st) == 0 && st.st_size > 0) {
      if(out_skipped) (*out_skipped)++; goto tick;
    }

    int fd = open(dest, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if(fd < 0) { if(out_failed) (*out_failed)++; goto tick; }
    int wrc = 0;
    ezip_stream *s = ezip_stream_open(a);
    if(s) {
      uint8_t rbuf[16 * 1024];
      ssize_t n;
      while((n = ezip_stream_read(s, rbuf, sizeof(rbuf))) > 0) {
        if(write(fd, rbuf, (size_t)n) != n) { wrc = -1; break; }
      }
      ezip_stream_close(s);
    } else {
      wrc = -1;
    }
    close(fd);
    if(wrc != 0) {
      unlink(dest);
      if(out_failed) (*out_failed)++;
    } else {
      if(out_added) (*out_added)++;
    }
  tick:
    pthread_mutex_lock(&g_dl.lock);
    g_dl.downloaded = out_added   ? *out_added   : 0;
    g_dl.skipped    = out_skipped ? *out_skipped : 0;
    g_dl.failed     = out_failed  ? *out_failed  : 0;
    pthread_mutex_unlock(&g_dl.lock);
  }
  ezip_close(a);
  free(zip);
  return 0;
}


static void*
download_all_thread_fn(void *arg) {
  (void)arg;
  syscall(SYS_thr_set_name, -1, "cheat-dl");

  char source[16];
  pthread_mutex_lock(&g_dl.lock);
  strncpy(source, g_dl.source, sizeof(source));
  source[sizeof(source)-1] = 0;
  pthread_mutex_unlock(&g_dl.lock);

  const cheat_zip_repo_t *list[CHEAT_ZIP_REPOS_N];
  int list_n = 0;
  int run_all = !source[0] || !strcasecmp(source, "all");
  if(run_all) {
    for(int i = 0; i < CHEAT_ZIP_REPOS_N; i++) list[list_n++] = &CHEAT_ZIP_REPOS[i];
  } else {
    list[list_n++] = cheat_zip_for(source);
  }

  ensure_cheats_dir();

  pthread_mutex_lock(&g_dl.lock);
  g_dl.total = g_dl.downloaded = g_dl.skipped = g_dl.failed = 0;
  g_dl.error[0] = 0;
  pthread_mutex_unlock(&g_dl.lock);

  size_t expected_cap = 8192;
  size_t expected_n   = 0;
  char (*expected)[96] = calloc(expected_cap, sizeof(*expected));

  int total_added = 0, total_skipped = 0, total_failed = 0, total_planned = 0;
  int repos_ok = 0, repos_failed = 0;

  for(int i = 0; i < list_n; i++) {
    int rc = process_one_repo_zip(list[i],
                                  &total_added, &total_skipped,
                                  &total_failed, &total_planned,
                                  expected, expected_cap, &expected_n);
    if(rc < 0) repos_failed++;
    else       repos_ok++;
  }

  pthread_mutex_lock(&g_dl.lock);
  strncpy(g_dl.current, "Verifying on-disk files…", sizeof(g_dl.current)-1);
  g_dl.current[sizeof(g_dl.current)-1] = 0;
  g_dl.missing_count = 0;
  pthread_mutex_unlock(&g_dl.lock);

  for(size_t i = 0; i < expected_n; i++) {
    const char *fn = expected[i];
    const char *ext = strrchr(fn, '.');
    const char *subdir =
        (ext && !strcasecmp(ext, ".json")) ? CHEATS_SUBDIR_JSON :
        (ext && !strcasecmp(ext, ".shn"))  ? CHEATS_SUBDIR_SHN  :
        (ext && !strcasecmp(ext, ".mc4"))  ? CHEATS_SUBDIR_MC4  :
                                             CHEATS_DIR;
    char path[MAX_CHEAT_FILEPATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", subdir, fn);
    struct stat st;
    if(stat(path, &st) != 0 || st.st_size <= 0) {
      pthread_mutex_lock(&g_dl.lock);
      if(g_dl.missing_count < DL_MAX_MISSING) {
        strncpy(g_dl.missing[g_dl.missing_count], fn,
                sizeof(g_dl.missing[g_dl.missing_count])-1);
        g_dl.missing[g_dl.missing_count][sizeof(g_dl.missing[g_dl.missing_count])-1] = 0;
      }
      g_dl.missing_count++;
      pthread_mutex_unlock(&g_dl.lock);
    }
  }
  free(expected);

  pthread_mutex_lock(&g_dl.lock);
  g_dl.verified = 1;
  if(repos_ok == 0 && total_added == 0 && total_skipped == 0) {
    snprintf(g_dl.error, sizeof(g_dl.error),
             "all %d repo download(s) failed (network / TLS / rate-limit)",
             repos_failed);
    dl_set_state_locked(DL_STATE_ERROR);
  } else {
    g_dl.current[0] = 0;
    dl_set_state_locked(DL_STATE_DONE);
  }
  pthread_mutex_unlock(&g_dl.lock);

  notify("Elf Arsenal: cheats — %d new, %d skipped, %d failed (%d/%d repos ok)",
         total_added, total_skipped, total_failed, repos_ok, list_n);

  return NULL;
}


/* Kick off the background download. */
static enum MHD_Result
download_all_start(struct MHD_Connection *conn) {
  const char *source = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "source");
  if(!source) source = "ps5cheats";
  if(strcasecmp(source, "ps5cheats")     != 0 &&
     strcasecmp(source, "goldhen")       != 0 &&
     strcasecmp(source, "hencollection") != 0) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                       "source must be 'ps5cheats', 'goldhen', or 'hencollection'");
  }

  pthread_mutex_lock(&g_dl.lock);
  if(g_dl.state == DL_STATE_RUNNING) {
    pthread_mutex_unlock(&g_dl.lock);
    return serve_error(conn, MHD_HTTP_CONFLICT,
                       "a repository download is already in progress; "
                       "wait for it to finish or check status");
  }
  /* Reset state for a new run. */
  memset(&g_dl.missing,    0, sizeof(g_dl.missing));
  memset(g_dl.error,       0, sizeof(g_dl.error));
  memset(g_dl.current,     0, sizeof(g_dl.current));
  strncpy(g_dl.source, source, sizeof(g_dl.source)-1);
  g_dl.source[sizeof(g_dl.source)-1] = 0;
  g_dl.total = 0;
  g_dl.downloaded = 0;
  g_dl.skipped = 0;
  g_dl.failed = 0;
  g_dl.verified = 0;
  g_dl.missing_count = 0;
  g_dl.started_at  = time(NULL);
  g_dl.finished_at = 0;
  g_dl.state = DL_STATE_RUNNING;
  pthread_mutex_unlock(&g_dl.lock);

  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  if(pthread_create(&t, &a, download_all_thread_fn, NULL) != 0) {
    pthread_attr_destroy(&a);
    pthread_mutex_lock(&g_dl.lock);
    snprintf(g_dl.error, sizeof(g_dl.error),
             "pthread_create failed: %s", strerror(errno));
    dl_set_state_locked(DL_STATE_ERROR);
    pthread_mutex_unlock(&g_dl.lock);
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                       "could not start download thread");
  }
  pthread_attr_destroy(&a);

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r,   "ok",     1);
  cJSON_AddStringToObject(r, "state",  "running");
  cJSON_AddStringToObject(r, "source", source);
  enum MHD_Result ret = serve_json_object(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


/* Snapshot the current progress as JSON so the UI can render the bar. */
static enum MHD_Result
download_all_status(struct MHD_Connection *conn) {
  cJSON *r = cJSON_CreateObject();

  pthread_mutex_lock(&g_dl.lock);
  const char *state_str =
    g_dl.state == DL_STATE_RUNNING ? "running" :
    g_dl.state == DL_STATE_DONE    ? "done"    :
    g_dl.state == DL_STATE_ERROR   ? "error"   :
                                     "idle";
  cJSON_AddStringToObject(r, "state",         state_str);
  cJSON_AddStringToObject(r, "source",        g_dl.source);
  cJSON_AddNumberToObject(r, "total",         g_dl.total);
  cJSON_AddNumberToObject(r, "downloaded",    g_dl.downloaded);
  cJSON_AddNumberToObject(r, "skipped",       g_dl.skipped);
  cJSON_AddNumberToObject(r, "failed",        g_dl.failed);
  cJSON_AddBoolToObject(r,   "verified",      g_dl.verified ? 1 : 0);
  cJSON_AddNumberToObject(r, "missingCount",  g_dl.missing_count);
  cJSON_AddStringToObject(r, "current",       g_dl.current);
  cJSON_AddNumberToObject(r, "startedAt",     (double)g_dl.started_at);
  cJSON_AddNumberToObject(r, "finishedAt",    (double)g_dl.finished_at);

  /* Up to DL_MAX_MISSING names so the UI can render examples. */
  int show = g_dl.missing_count;
  if(show > DL_MAX_MISSING) show = DL_MAX_MISSING;
  cJSON *missing = cJSON_AddArrayToObject(r, "missing");
  for(int i=0; i<show; i++) {
    cJSON_AddItemToArray(missing, cJSON_CreateString(g_dl.missing[i]));
  }
  if(g_dl.error[0]) {
    cJSON_AddStringToObject(r, "error", g_dl.error);
  }
  pthread_mutex_unlock(&g_dl.lock);

  enum MHD_Result ret = serve_json_object(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


/*  /api/cr/eboot — dump decrypted eboot from running game memory         */

/* Read the decrypted eboot from the currently running game's process memory
   via ptrace and cache it to /data/cheatrunner/eboot/<titleId>/eboot.dec.
   CheatRunner can then open this file to verify cheat offsets statically,
   eliminating the absolute/relative address ambiguity for SHN/MC4 patches. */
static enum MHD_Result
serve_cr_eboot(struct MHD_Connection *conn) {
  const char *title_id = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "title");
  if(!title_id || !is_safe_title_id(title_id))
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "bad title param");

  char cache_dir[280], cache_path[320];
  snprintf(cache_dir,  sizeof(cache_dir),
           "/data/cheatrunner/eboot/%s", title_id);
  snprintf(cache_path, sizeof(cache_path), "%s/eboot.dec", cache_dir);

  /* Return cached copy immediately if present. */
  struct stat st;
  if(stat(cache_path, &st) == 0 && st.st_size > 0x40) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r,   "ok",     1);
    cJSON_AddStringToObject(r, "path",   cache_path);
    cJSON_AddNumberToObject(r, "size",   (double)st.st_size);
    cJSON_AddBoolToObject(r,   "cached", 1);
    enum MHD_Result ret = serve_json_object(conn, MHD_HTTP_OK, r);
    cJSON_Delete(r);
    return ret;
  }

  /* Game must be running — we read the decrypted ELF straight from memory. */
  pid_t pid = -1;
  intptr_t base = 0;
  char running[16] = {0};
  if(get_running_game(&pid, running, sizeof(running), &base) != 0 || base <= 0)
    return serve_error(conn, MHD_HTTP_SERVICE_UNAVAILABLE,
                       "no game running — launch the game first");

  char run_norm[12], want_norm[12];
  if(!title_id_normalize(running, run_norm) ||
     !title_id_normalize(title_id, want_norm) ||
     strcmp(run_norm, want_norm) != 0)
    return serve_error(conn, MHD_HTTP_CONFLICT,
                       "running game does not match requested title");

  if(pt_attach(pid) < 0)
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "pt_attach failed");

  /* Read ELF64 header at image base. */
  uint8_t ehdr[64];
  if(pt_copyout(pid, base, ehdr, sizeof(ehdr)) < 0 ||
     memcmp(ehdr, "\x7f""ELF", 4) != 0) {
    pt_detach(pid, 0);
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                       "could not read ELF header at image base");
  }

  uint64_t e_phoff     = *(uint64_t*)(ehdr + 32);
  uint16_t e_phentsize = *(uint16_t*)(ehdr + 54);
  uint16_t e_phnum     = *(uint16_t*)(ehdr + 56);

  if(e_phnum == 0 || e_phnum > 64 || e_phentsize < 56) {
    pt_detach(pid, 0);
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                       "invalid ELF program header count/size");
  }

  /* Read program headers to find the total mapped extent. */
  size_t phdrs_sz = (size_t)e_phnum * e_phentsize;
  uint8_t *phdrs = malloc(phdrs_sz);
  if(!phdrs) { pt_detach(pid, 0); return serve_error(conn, 500, "oom"); }

  if(pt_copyout(pid, base + (intptr_t)e_phoff, phdrs, phdrs_sz) < 0) {
    free(phdrs); pt_detach(pid, 0);
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                       "could not read program headers");
  }

  intptr_t map_end = base;
  for(int i = 0; i < e_phnum; i++) {
    uint8_t *ph      = phdrs + i * e_phentsize;
    uint32_t p_type  = *(uint32_t*)(ph +  0);
    uint64_t p_vaddr = *(uint64_t*)(ph + 16);
    uint64_t p_filesz= *(uint64_t*)(ph + 32);
    if(p_type != 1 /* PT_LOAD */ || p_filesz == 0) continue;
    intptr_t seg_end = base + (intptr_t)p_vaddr + (intptr_t)p_filesz;
    if(seg_end > map_end) map_end = seg_end;
  }
  free(phdrs);

  size_t total = (size_t)(map_end - base);
  if(total < 0x1000 || total > 256u * 1024 * 1024) {
    pt_detach(pid, 0);
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                       "ELF mapped size out of expected range");
  }

  uint8_t *elf_buf = malloc(total);
  if(!elf_buf) { pt_detach(pid, 0); return serve_error(conn, 500, "oom"); }

  /* Read in 64 KB chunks to avoid ptrace size limits. */
  size_t off = 0;
  int read_ok = 1;
  while(off < total) {
    size_t chunk = total - off;
    if(chunk > 0x10000) chunk = 0x10000;
    if(pt_copyout(pid, base + (intptr_t)off, elf_buf + off, chunk) < 0) {
      read_ok = 0; break;
    }
    off += chunk;
  }
  pt_detach(pid, 0);

  if(!read_ok) {
    free(elf_buf);
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                       "memory read failed mid-dump");
  }

  mkdir("/data/cheatrunner",       0755);
  mkdir("/data/cheatrunner/eboot", 0755);
  mkdir(cache_dir,                 0755);

  int fd = open(cache_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if(fd < 0) {
    free(elf_buf);
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                       "could not open cache file for write");
  }
  ssize_t written = write(fd, elf_buf, total);
  close(fd);
  free(elf_buf);

  if(written != (ssize_t)total) {
    unlink(cache_path);
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                       "cache write incomplete");
  }

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r,   "ok",     1);
  cJSON_AddStringToObject(r, "path",   cache_path);
  cJSON_AddNumberToObject(r, "size",   (double)total);
  cJSON_AddBoolToObject(r,   "cached", 0);
  enum MHD_Result ret = serve_json_object(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


/*  Top-level dispatcher                                                  */

enum MHD_Result
cheats_request(struct MHD_Connection *conn, const char *url,
               const char *method, const char *upload_data,
               size_t *upload_data_size, void **con_cls) {
  (void)upload_data;
  (void)upload_data_size;
  (void)con_cls;

  if(strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
    return serve_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED,
                       "use GET; uploads go through FTP (port 2121) into "
                       "/data/elf-arsenal/cheats/");
  }

  if(!strcmp(url, "/api/cheats")) {
    const char *title_id = MHD_lookup_connection_value(conn,
                                MHD_GET_ARGUMENT_KIND, "titleId");
    if(title_id) {
      return get_cheats_for(conn, title_id);
    }
    return list_cheats(conn);
  }
  if(!strcmp(url, "/api/cheats/toggle")) {
    return toggle_cheat(conn);
  }
  if(!strcmp(url, "/api/cheats/delete")) {
    const char *title_id = MHD_lookup_connection_value(conn,
                                MHD_GET_ARGUMENT_KIND, "titleId");
    return delete_cheats_for(conn, title_id);
  }
  if(!strcmp(url, "/api/cheats/download")) {
    return download_cheats(conn);
  }
  if(!strcmp(url, "/api/cheats/download-all")) {
    return download_all_start(conn);
  }
  if(!strcmp(url, "/api/cheats/download-all/status")) {
    return download_all_status(conn);
  }
  if(!strcmp(url, "/api/cr/eboot")) {
    return serve_cr_eboot(conn);
  }
  return serve_error(conn, MHD_HTTP_NOT_FOUND, "no such endpoint");
}
