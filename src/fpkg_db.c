#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <sqlite3.h>

#include "fpkg_db.h"

#define APP_DB_PATH    "/system_data/priv/mms/app.db"
#define APPINFO_DB_PATH "/system_data/priv/mms/appinfo.db"
#define APP_DIR        "/user/app"

/* Pre-rebuild snapshots of the system's own launchable rows. Live on /data,
   which survives a DB rebuild. Restoring these verbatim sidesteps having to
   synthesise the PSN concept id (which the rebuild wipes and we can't resolve
   offline).

   Two rotating sets:
     current/   — updated on every change (latest registration).
     previous/  — snapshot of current taken right BEFORE each new install is
                  folded in, i.e. the last working state without the newest
                  game. If a fresh install corrupts the DB, restore from here
                  to roll back exactly one install. */
#define BACKUP_ROOT            "/data/FPKGDBBCKUP"
#define BACKUP_CUR_APP_DB      BACKUP_ROOT "/current/app.db"
#define BACKUP_CUR_APPINFO_DB  BACKUP_ROOT "/current/appinfo.db"
#define BACKUP_PREV_APP_DB     BACKUP_ROOT "/previous/app.db"
#define BACKUP_PREV_APPINFO_DB BACKUP_ROOT "/previous/appinfo.db"

/* Backup set the current repair restores from (selected by fpkg_db_repair_src).
   Worker-thread only — plain statics are fine. */
static char g_bk_app[128]     = BACKUP_CUR_APP_DB;
static char g_bk_appinfo[128] = BACKUP_CUR_APPINFO_DB;

/* Sony's read-only offline title->concept map (23k PS4 titles). Lets us
   resolve the real PSN concept id with no network and no PSN sign-in. */
#define CONCEPT_TITLE_DB  "/system/priv/mms_ro/concept_title.db"
#define APPMETA_USER   "/user/appmeta"
#define APPMETA_PRIV   "/system_data/priv/appmeta"

#define EXT0_APP_DIR   "/mnt/ext0/user/app"   /* USB extended storage */
#define EXT1_APP_DIR   "/mnt/ext1/user/app"   /* M.2 / NVMe extended storage */

/* DLC, patch, and playgo dirs — same CUSA subfolder layout as app. */
#define ADDCONT_DIR      "/user/addcont"
#define PATCH_DIR        "/user/patch"
#define PLAYGO_DIR       "/user/playgo"
#define EXT0_ADDCONT_DIR "/mnt/ext0/user/addcont"
#define EXT0_PATCH_DIR   "/mnt/ext0/user/patch"
#define EXT0_PLAYGO_DIR  "/mnt/ext0/user/playgo"
#define EXT1_ADDCONT_DIR "/mnt/ext1/user/addcont"
#define EXT1_PATCH_DIR   "/mnt/ext1/user/patch"
#define EXT1_PLAYGO_DIR  "/mnt/ext1/user/playgo"

/* Where a title's real app.pkg lives. USB (ext0) titles are re-registered by
   the system itself on mount (it re-derives the per-device id from the
   encrypted partition), so we never synthesise those rows. M.2 (ext1) is NOT
   restored by the system, and its row needs no per-device id, so we can. */
enum { STORE_NONE = 0, STORE_INTERNAL, STORE_M2, STORE_USB };

/* Footprint the system records on internal storage for an M.2-resident title
   (metadata/playgo). Observed constant on retail. */
#define M2_SIZE_OTHER_HDD  10485760LL

/* Fixed magic values observed identically across cid:scp and cid:local
   installs (install-source independent). */
#define INSTALL_VERSION    311029853560242177LL  /* observed on current retail FW */
#define PATH_ICON0_MAGIC   (-8070450532247928832LL)
#define PATH_PIC0_MAGIC    (-6917529027641081856LL)
#define PRIMARY_TITLE_SORT 132101LL
#define ICON_PRIMARY_SORT  4295099397LL          /* (1<<32)|132101 */


typedef struct {
  char tid[16];
  char content_id[64];
  char title[256];
  char category[8];
  char app_ver[16];
  char version[16];
  char format[8];
  long long app_type, attribute, attribute2, parental, system_ver;
  long long category_type, dl_size, rp_key;
  long long size;
  long long idx;
  long long concept_id;   /* real PSN concept (from concept_title.db), 0 if none */
  char now[32];
  long long icon_ts, pic_ts;
  char deeplink[64];
  char huburi[96];
  char local_concept[40];
  char meta_path[64];
  char org_path[64];
  char icon0info[160];
  char pic0info[160];
  int  has_pic0;
  int  is_app;          /* viewCategory "app" (CATEGORY gde) vs "game" */
  int  storage;         /* STORE_* — where the real app.pkg lives */
  char app_root[64];    /* dir that holds <tid>/app.pkg */
} title_info;


/* ------------------------------------------------------------------ SFO */

static const uint8_t* sfo_find(const uint8_t *d, size_t len, const char *key,
                               uint16_t *fmt_out, uint32_t *vlen_out) {
  if(len < 20 || memcmp(d, "\0PSF", 4) != 0) return NULL;
  uint32_t key_tab  = *(const uint32_t*)(d + 8);
  uint32_t data_tab = *(const uint32_t*)(d + 12);
  uint32_t n        = *(const uint32_t*)(d + 16);
  for(uint32_t i = 0; i < n; i++) {
    const uint8_t *e = d + 20 + i*16;
    if((size_t)(e + 16 - d) > len) break;
    uint16_t key_off = *(const uint16_t*)(e + 0);
    uint16_t fmt     = *(const uint16_t*)(e + 2);
    uint32_t vlen    = *(const uint32_t*)(e + 4);
    uint32_t data_off= *(const uint32_t*)(e + 12);
    const char *k = (const char*)(d + key_tab + key_off);
    if(key_tab + key_off >= len) continue;
    if(!strcmp(k, key)) {
      if(fmt_out) *fmt_out = fmt;
      if(vlen_out) *vlen_out = vlen;
      if(data_tab + data_off > len) return NULL;
      return d + data_tab + data_off;
    }
  }
  return NULL;
}

static void sfo_str(const uint8_t *d, size_t len, const char *key,
                    char *out, size_t outsz) {
  out[0] = 0;
  uint16_t fmt; uint32_t vlen;
  const uint8_t *v = sfo_find(d, len, key, &fmt, &vlen);
  if(!v) return;
  size_t n = 0;
  while(n < outsz-1 && v[n]) { out[n] = (char)v[n]; n++; }
  out[n] = 0;
}

static long long sfo_int(const uint8_t *d, size_t len, const char *key,
                         long long dflt) {
  uint16_t fmt; uint32_t vlen;
  const uint8_t *v = sfo_find(d, len, key, &fmt, &vlen);
  if(!v || fmt != 0x0404) return dflt;
  return (long long)(*(const uint32_t*)v);
}


/* -------------------------------------------------------- AppInfoJson */

typedef struct { char *buf; size_t cap, len; int first; } cbuf;

static void cb_raw(cbuf *b, const char *s) {
  size_t n = strlen(s);
  if(b->len + n + 1 > b->cap) {
    size_t nc = (b->cap ? b->cap*2 : 8192);
    while(nc < b->len + n + 1) nc *= 2;
    char *nb = realloc(b->buf, nc);
    if(!nb) return;
    b->buf = nb; b->cap = nc;
  }
  memcpy(b->buf + b->len, s, n);
  b->len += n; b->buf[b->len] = 0;
}

/* Append a JSON-escaped string value. */
static void cb_jstr(cbuf *b, const char *s) {
  char tmp[1024]; size_t o = 0;
  for(; *s && o < sizeof(tmp)-2; s++) {
    if(*s == '"' || *s == '\\') tmp[o++] = '\\';
    tmp[o++] = *s;
  }
  tmp[o] = 0;
  cb_raw(b, tmp);
}

static void aij_comma(cbuf *b) { if(!b->first) cb_raw(b, ","); b->first = 0; }

static void aij_int(cbuf *b, const char *key, long long data) {
  char h[128];
  aij_comma(b);
  snprintf(h, sizeof(h), "{\"data\":%lld,\"key\":\"%s\",\"size\":8,\"type\":0}",
           data, key);
  cb_raw(b, h);
}

static void aij_str(cbuf *b, const char *key, const char *data) {
  if(!data) data = "";
  aij_comma(b);
  cb_raw(b, "{\"data\":\"");
  cb_jstr(b, data);
  char h[96];
  snprintf(h, sizeof(h), "\",\"key\":\"%s\",\"size\":%u,\"type\":2}",
           key, (unsigned)strlen(data));
  cb_raw(b, h);
}

/* The METADATA_ID the launcher uses per storage type.
   Internal uses "normal:internal:0" (current install).
   M.2 uses "prior:internal:-1" — the system always writes this for M.2 games
   (the _m2_device_id=-1 + _org_path=/mnt/ext1/... set is how the launcher
   finds the game on M.2). */
static void metadata_id_str(const title_info *ti, char *out, size_t n) {
  snprintf(out, n, "%s", ti->storage == STORE_M2 ? "prior:internal:-1"
                                                  : "normal:internal:0");
}

/* A field sink lets one canonical field list (emit_fields) drive both the
   app.db AppInfoJson blob and the appinfo.db tbl_appinfo key/val rows. */
typedef struct {
  void (*ei)(void *ctx, const char *key, long long v);
  void (*es)(void *ctx, const char *key, const char *v);
  void  *ctx;
} field_sink;

/* The full per-title field set. Emitted in the same order the system uses. */
static void emit_fields(const title_info *ti, const field_sink *s) {
  char mdid[48]; metadata_id_str(ti, mdid, sizeof(mdid));
  long long m2  = (ti->storage == STORE_M2) ? -1LL : 0LL;
  long long oth = (ti->storage == STORE_M2) ? M2_SIZE_OTHER_HDD : 0LL;
  s->ei(s->ctx, "#_access_index", ti->idx);
  s->ei(s->ctx, "#_contents_status", 0);
  s->es(s->ctx, "#_ctime", ti->now);
  s->es(s->ctx, "#_install_time", ti->now);
  s->es(s->ctx, "#_last_access_time", ti->now);
  s->es(s->ctx, "#_mtime", ti->now);
  s->es(s->ctx, "#_promote_time", ti->now);
  s->ei(s->ctx, "#_size", ti->size);
  s->ei(s->ctx, "#exit_type", 0);
  s->ei(s->ctx, "APP_TYPE", ti->app_type);
  s->es(s->ctx, "APP_VER", ti->app_ver);
  s->ei(s->ctx, "ATTRIBUTE", ti->attribute);
  s->ei(s->ctx, "ATTRIBUTE2", ti->attribute2);
  s->ei(s->ctx, "ATTRIBUTE_INTERNAL", 0);
  s->es(s->ctx, "CATEGORY", ti->category);
  s->ei(s->ctx, "CATEGORY_TYPE", ti->category_type);
  if(ti->concept_id > 0) s->ei(s->ctx, "CONCEPT_ID", ti->concept_id);
  s->es(s->ctx, "CONTENT_ID", ti->content_id);
  s->es(s->ctx, "DEEPLINK_URI", ti->deeplink);
  s->ei(s->ctx, "DISPLAYLOCATION", 138);
  s->ei(s->ctx, "DISP_LOCATION_1", 0);
  s->ei(s->ctx, "DISP_LOCATION_2", 0);
  s->ei(s->ctx, "DOWNLOAD_DATA_SIZE", ti->dl_size);
  s->es(s->ctx, "FORMAT", ti->format);
  s->es(s->ctx, "HUBAPP_URI", ti->huburi);
  s->es(s->ctx, "METADATA_ID", mdid);
  s->ei(s->ctx, "PARENTAL_LEVEL", ti->parental);
  s->ei(s->ctx, "PT_PARAM", 0);
  s->ei(s->ctx, "PUBTOOL_VERSION", 0);
  s->ei(s->ctx, "REMOTE_PLAY_KEY_ASSIGN", ti->rp_key);
  s->ei(s->ctx, "SERVICE_ID_ADDCONT_ADD_1", 0);
  s->ei(s->ctx, "SERVICE_ID_ADDCONT_ADD_2", 0);
  s->ei(s->ctx, "SERVICE_ID_ADDCONT_ADD_3", 0);
  s->ei(s->ctx, "SERVICE_ID_ADDCONT_ADD_4", 0);
  s->ei(s->ctx, "SERVICE_ID_ADDCONT_ADD_5", 0);
  s->ei(s->ctx, "SERVICE_ID_ADDCONT_ADD_6", 0);
  s->ei(s->ctx, "SERVICE_ID_ADDCONT_ADD_7", 0);
  s->ei(s->ctx, "SYSTEM_VER", ti->system_ver);
  s->es(s->ctx, "TITLE", ti->title);
  s->es(s->ctx, "TITLE_ID", ti->tid);
  s->ei(s->ctx, "USER_DEFINED_PARAM_1", 0);
  s->ei(s->ctx, "USER_DEFINED_PARAM_2", 0);
  s->ei(s->ctx, "USER_DEFINED_PARAM_3", 0);
  s->ei(s->ctx, "USER_DEFINED_PARAM_4", 0);
  s->es(s->ctx, "VERSION", ti->version);
  s->ei(s->ctx, "_app_format_type", 0);
  s->ei(s->ctx, "_contents_ext_type", 0);
  s->ei(s->ctx, "_contents_location", 0);
  s->ei(s->ctx, "_current_slot", 0);
  s->ei(s->ctx, "_disable_live_detail", 0);
  s->ei(s->ctx, "_external_hdd_app_status", 0);
  s->ei(s->ctx, "_hdd_location", 0);
  s->ei(s->ctx, "_install_status", 0);
  s->ei(s->ctx, "_install_sub_status", 1);
  s->ei(s->ctx, "_install_version", INSTALL_VERSION);
  s->es(s->ctx, "_local_concept_id", ti->local_concept);
  s->ei(s->ctx, "_m2_device_id", m2);
  s->es(s->ctx, "_metadata_path", ti->meta_path);
  s->ei(s->ctx, "_not_install_sub_status", 0);
  s->es(s->ctx, "_org_path", ti->org_path);
  s->ei(s->ctx, "_path_changeinfo_info", 0);
  s->ei(s->ctx, "_path_icon0_info", PATH_ICON0_MAGIC);
  s->ei(s->ctx, "_path_icon0_info_time_stamp", ti->icon_ts);
  s->ei(s->ctx, "_path_info", 0);
  s->ei(s->ctx, "_path_info_2", 0);
  s->ei(s->ctx, "_path_pic0_info", PATH_PIC0_MAGIC);
  s->ei(s->ctx, "_path_pic0_info_time_stamp", ti->pic_ts);
  s->ei(s->ctx, "_path_pic1_info", 0);
  s->ei(s->ctx, "_path_pic1_info_time_stamp", 0);
  s->ei(s->ctx, "_path_promotion0_info", 0);
  s->ei(s->ctx, "_primary_title_sort", PRIMARY_TITLE_SORT);
  s->ei(s->ctx, "_ps_platform", 1);
  s->ei(s->ctx, "_size_other_hdd", oth);
  s->ei(s->ctx, "_sort_priority", 100);
  s->ei(s->ctx, "_uninstallable", 1);
  s->ei(s->ctx, "_view_category", 0);
  s->ei(s->ctx, "sync_index", ti->idx);
}

/* ---- app.db AppInfoJson sink ---- */
static void json_emit_int(void *ctx, const char *k, long long v) { aij_int((cbuf*)ctx, k, v); }
static void json_emit_str(void *ctx, const char *k, const char *v){ aij_str((cbuf*)ctx, k, v); }

/* Build the full AppInfoJson blob. Returns a malloc'd string (caller frees). */
static char* build_appinfo(const title_info *ti) {
  cbuf bb = {0}; cbuf *b = &bb;
  cb_raw(b, "{\"field_list\":[");
  b->first = 1;
  field_sink s = { json_emit_int, json_emit_str, b };
  emit_fields(ti, &s);
  cb_raw(b, "],\"mimeType\":0,\"mimeTypeFormatVersion\":0,"
            "\"parserId\":0,\"promoterFormatVersion\":0}");
  return b->buf;
}


/* ---------------------------------------------------------- derivation */

static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static long long file_mtime(const char *p) {
  struct stat st;
  if(stat(p, &st) == 0) return (long long)st.st_mtime;
  return 0;
}
static long long file_size(const char *p) {
  struct stat st;
  if(stat(p, &st) == 0) return (long long)st.st_size;
  return 0;
}

/* Find where a title's real app.pkg lives. Probe order M.2, internal, USB.
   An empty stub dir (data wiped by a DB rebuild) has no app.pkg anywhere and
   returns STORE_NONE. */
static int detect_storage(const char *tid, char *root_out, size_t root_sz) {
  char pkg[256];
  snprintf(pkg, sizeof(pkg), "%s/%s/app.pkg", EXT1_APP_DIR, tid);
  if(file_size(pkg) > 0) { snprintf(root_out, root_sz, "%s", EXT1_APP_DIR); return STORE_M2; }
  snprintf(pkg, sizeof(pkg), "%s/%s/app.pkg", APP_DIR, tid);
  if(file_size(pkg) > 0) { snprintf(root_out, root_sz, "%s", APP_DIR); return STORE_INTERNAL; }
  snprintf(pkg, sizeof(pkg), "%s/%s/app.pkg", EXT0_APP_DIR, tid);
  if(file_size(pkg) > 0) { snprintf(root_out, root_sz, "%s", EXT0_APP_DIR); return STORE_USB; }
  return STORE_NONE;
}

static void now_string(char *out, size_t n) {
  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tm; localtime_r(&ts.tv_sec, &tm);
  snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
           tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
           tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ts.tv_nsec/1000000));
}

/* Read the title's registered app-dir path (_org_path) from the live app.db
   AppInfoJson, into `out` (unescaped). Empty if the title has no live row. This
   is where the system currently believes the game lives; if the pkg is actually
   there the registration is valid, if not it's stale (the game was moved while
   our 0555 lock blocked the move's source cleanup, or wiped by a rebuild). */
static void reg_org_path(sqlite3 *db, const char *tid, char *out, size_t osz) {
  out[0] = 0;
  sqlite3_stmt *st = NULL;
  if(sqlite3_prepare_v2(db, "SELECT AppInfoJson FROM tbl_contentinfo WHERE titleId=?1 LIMIT 1",
                        -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(st) == SQLITE_ROW) {
      const char *j = (const char*)sqlite3_column_text(st, 0);
      const char *k = j ? strstr(j, "\"key\":\"_org_path\"") : NULL;
      if(k) {
        /* fields are {"data":"<path>","key":"_org_path",...} — the value is the
           nearest preceding "data":" before this key. */
        const char *d = k;
        while(d > j && strncmp(d, "\"data\":\"", 8) != 0) d--;
        if(strncmp(d, "\"data\":\"", 8) == 0) {
          d += 8;
          size_t o = 0;
          while(*d && *d != '"' && o < osz - 1) {
            if(d[0] == '\\' && d[1] == '/') { out[o++] = '/'; d += 2; }
            else out[o++] = *d++;
          }
          out[o] = 0;
        }
      }
    }
  }
  sqlite3_finalize(st);
}

/* Resolve a PS4 title's real PSN concept id from Sony's offline map. Returns
   the numeric concept id, or 0 if the title isn't listed (e.g. homebrew). */
static long long concept_lookup(const char *tid) {
  sqlite3 *cdb = NULL;
  long long cid = 0;
  if(sqlite3_open_v2(CONCEPT_TITLE_DB, &cdb,
                     SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
    if(cdb) sqlite3_close(cdb);
    return 0;
  }
  sqlite3_stmt *st = NULL;
  if(sqlite3_prepare_v2(cdb, "SELECT concept_id FROM tbl_concept_title "
                             "WHERE title_id=?1 LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(st) == SQLITE_ROW) {
      const char *c = (const char*)sqlite3_column_text(st, 0);  /* stored as text */
      if(c) cid = strtoll(c, NULL, 10);
    }
  }
  sqlite3_finalize(st);
  sqlite3_close(cdb);
  return cid;
}

/* Populate ti from /system_data/priv/appmeta/<tid>/param.sfo + on-disk art.
   Returns 0 on success, -1 if the SFO is missing/unreadable. */
static int load_title_info(const char *tid, long long idx, int storage,
                           const char *app_root, title_info *ti) {
  memset(ti, 0, sizeof(*ti));
  ti->storage = storage;
  snprintf(ti->app_root, sizeof(ti->app_root), "%s", app_root);
  snprintf(ti->tid, sizeof(ti->tid), "%s", tid);
  ti->idx = idx;
  now_string(ti->now, sizeof(ti->now));

  char sfo_path[128];
  snprintf(sfo_path, sizeof(sfo_path), "%s/%s/param.sfo", APPMETA_PRIV, tid);
  FILE *f = fopen(sfo_path, "rb");
  if(!f) return -1;
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  if(sz <= 0 || sz > 1024*1024) { fclose(f); return -1; }
  uint8_t *d = malloc((size_t)sz);
  if(!d) { fclose(f); return -1; }
  if(fread(d, 1, (size_t)sz, f) != (size_t)sz) { free(d); fclose(f); return -1; }
  fclose(f);

  sfo_str(d, sz, "CONTENT_ID", ti->content_id, sizeof(ti->content_id));
  sfo_str(d, sz, "TITLE",      ti->title,      sizeof(ti->title));
  sfo_str(d, sz, "CATEGORY",   ti->category,   sizeof(ti->category));
  sfo_str(d, sz, "APP_VER",    ti->app_ver,    sizeof(ti->app_ver));
  sfo_str(d, sz, "VERSION",    ti->version,    sizeof(ti->version));
  sfo_str(d, sz, "FORMAT",     ti->format,     sizeof(ti->format));
  ti->app_type   = sfo_int(d, sz, "APP_TYPE", 1);
  ti->attribute  = sfo_int(d, sz, "ATTRIBUTE", 0);
  ti->attribute2 = sfo_int(d, sz, "ATTRIBUTE2", 0);
  ti->parental   = sfo_int(d, sz, "PARENTAL_LEVEL", 0);
  ti->system_ver = sfo_int(d, sz, "SYSTEM_VER", 0);
  ti->dl_size    = sfo_int(d, sz, "DOWNLOAD_DATA_SIZE", 0);
  ti->rp_key     = sfo_int(d, sz, "REMOTE_PLAY_KEY_ASSIGN", 0);
  free(d);

  if(!ti->title[0] || !ti->content_id[0]) return -1;
  if(!ti->category[0]) snprintf(ti->category, sizeof(ti->category), "gd");

  ti->is_app = (strcmp(ti->category, "gde") == 0);
  ti->category_type = 61440;   /* gd/gde/gp games + apps observed as 61440 */

  snprintf(ti->deeplink, sizeof(ti->deeplink), "psgm:play?id=%s", tid);
  snprintf(ti->huburi, sizeof(ti->huburi),
           ti->is_app ? "psmediahub:main?titleId=%s" : "pshome:gamehub?titleId=%s", tid);
  ti->concept_id = concept_lookup(tid);
  if(ti->concept_id > 0)
    snprintf(ti->local_concept, sizeof(ti->local_concept),
             "cid:scp:%016llx", ti->concept_id);
  else
    snprintf(ti->local_concept, sizeof(ti->local_concept), "cid:local:%s", tid);
  snprintf(ti->meta_path, sizeof(ti->meta_path), "%s/%s", APPMETA_USER, tid);
  snprintf(ti->org_path, sizeof(ti->org_path), "%s/%s", app_root, tid);

  char pkg[160];
  snprintf(pkg, sizeof(pkg), "%s/%s/app.pkg", app_root, tid);
  ti->size = file_size(pkg);

  char icon[160], pic_dds[160], pic_png[160];
  snprintf(icon,    sizeof(icon),    "%s/%s/icon0.png", APPMETA_USER, tid);
  snprintf(pic_dds, sizeof(pic_dds), "%s/%s/pic0.dds",  APPMETA_USER, tid);
  snprintf(pic_png, sizeof(pic_png), "%s/%s/pic0.png",  APPMETA_USER, tid);
  ti->icon_ts = file_mtime(icon);
  if(!ti->icon_ts) ti->icon_ts = (long long)time(NULL);
  snprintf(ti->icon0info, sizeof(ti->icon0info),
           "%s/%s/icon0.png?ts=%lld", APPMETA_USER, tid, ti->icon_ts);
  if(file_exists(pic_dds)) {
    ti->has_pic0 = 1; ti->pic_ts = file_mtime(pic_dds);
    snprintf(ti->pic0info, sizeof(ti->pic0info),
             "%s/%s/pic0.dds?ts=%lld", APPMETA_USER, tid, ti->pic_ts);
  } else if(file_exists(pic_png)) {
    ti->has_pic0 = 1; ti->pic_ts = file_mtime(pic_png);
    snprintf(ti->pic0info, sizeof(ti->pic0info),
             "%s/%s/pic0.png?ts=%lld", APPMETA_USER, tid, ti->pic_ts);
  }
  if(!ti->pic_ts) ti->pic_ts = ti->icon_ts;
  return 0;
}


/* ------------------------------------------------------------- inserts */

static int run_sql(sqlite3 *db, char *sql) {   /* takes ownership: frees sql */
  if(!sql) return -1;
  char *err = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
  if(rc != SQLITE_OK) {
    fprintf(stderr, "fpkg_db: sql error: %s\n", err ? err : "(?)");
    sqlite3_free(err);
  }
  sqlite3_free(sql);
  return rc == SQLITE_OK ? 0 : -1;
}

static int insert_contentinfo(sqlite3 *db, const title_info *ti) {
  char *aij = build_appinfo(ti);
  if(!aij) return -1;
  const char *vc  = ti->is_app ? "app" : "game";
  const char *pic = ti->has_pic0 ? ti->pic0info : NULL;   /* %Q -> NULL */
  long long size_other = (ti->storage == STORE_M2) ? M2_SIZE_OTHER_HDD : 0LL;
  int pkg_loc = (ti->storage == STORE_M2) ? 3
              : (ti->storage == STORE_USB) ? 2 : 1;
  char *sql = sqlite3_mprintf(
    "INSERT OR REPLACE INTO tbl_contentinfo "
    "(titleId,contentId,titleName,metaDataPath,lastAccessTime,contentStatus,"
    "contentLocation,ps4parentalLevel,sortPriority,pathInfo,lastAccessIndex,"
    "dispLocation,uninstallable,ps4contentType,pathInfo2,size,promoteTime,"
    "installTime,viewCategory,ps4disableLiveDetail,hddLocation,"
    "externalHddAppStatus,mTime,sizeOtherHdd,appFormatType,categoryType,"
    "ps4Attribute,attributeInternal,conceptId,localConceptId,pprDeeplinkUri,"
    "pprHubAppUri,platform,installStatus,icon0Info,pic0Info,discStatus,appType,"
    "primaryTitleSort,appinfoContentType,contentBadgeType,is3rdPartyVideoApp,"
    "packageLocation,AppInfoJson) VALUES "
    "(%Q,%Q,%Q,%Q,%Q,0,0,%lld,100,0,%lld,138,1,0,0,%lld,%Q,%Q,%Q,0,0,0,%Q,%lld,0,"
    "%lld,%lld,0,%lld,%Q,%Q,%Q,1,0,%Q,%Q,0,1,%lld,1,0,0,%d,%Q)",
    ti->tid, ti->content_id, ti->title, ti->meta_path, ti->now,
    ti->parental, ti->idx, ti->size, ti->now, ti->now, vc, ti->now,
    size_other, ti->category_type, ti->attribute, ti->concept_id, ti->local_concept,
    ti->deeplink, ti->huburi, ti->icon0info, pic,
    PRIMARY_TITLE_SORT, pkg_loc, aij);
  free(aij);
  return run_sql(db, sql);
}

static int insert_iconinfo(sqlite3 *db, const char *suffix, const title_info *ti) {
  char *sql = sqlite3_mprintf(
    "INSERT OR REPLACE INTO tbl_iconinfo_%s "
    "(titleId,titleName,localConceptId,conceptId,lastAccessTime,lastAccessIndex,"
    "recentActivityDatePlayedOrInstalled,installedDate,installStatus,dispLocation,"
    "visible,externalLaunchFlag,psNowFlag,deeplinkUri,hubAppUri,platform,"
    "appDrmType,betaFlag,contentBadgeType,discStatus,primaryTitleSort,contentId) "
    "VALUES (%Q,%Q,%Q,%lld,%Q,%lld,%Q,%Q,2,138,1,0,0,%Q,%Q,1,5,0,0,0,%lld,%Q)",
    suffix, ti->tid, ti->title, ti->local_concept, ti->concept_id, ti->now, ti->idx,
    ti->now, ti->now, ti->deeplink, ti->huburi, ICON_PRIMARY_SORT, ti->content_id);
  return run_sql(db, sql);
}

static int insert_concepticoninfo(sqlite3 *db, const char *suffix, const title_info *ti) {
  const char *pic = ti->has_pic0 ? ti->pic0info : NULL;
  char *sql = sqlite3_mprintf(
    "INSERT OR REPLACE INTO tbl_concepticoninfo_%s "
    "(localConceptId,validFlag,conceptId,conceptName,lastAccessTime,lastAccessIndex,"
    "totalPlayedTime,recentActivityDatePlayedOrInstalled,primaryTitleId,"
    "primaryTitleName,primaryTitlePlatform,lastInteractedTime,dispLocation,"
    "installedStatus,installedStatusPerPlatform,entitlementStatus,"
    "filterTypeEntitlement,filterTypeHidden,totalEstimatedSize,promoteTime,"
    "installTime,psNowFlag,deeplinkUri,hubAppUri,metaDataPath,icon0Info,pic0Info,"
    "platform,minimumAge,genreFlags,spaceType,packageLocation) "
    "VALUES (%Q,1,%lld,%Q,%Q,%lld,0,%Q,%Q,%Q,1,%Q,1162,1,2,0,0,1,0,%Q,%Q,0,%Q,%Q,%Q,%Q,%Q,"
    "2,0,0,0,'3')",
    suffix, ti->local_concept, ti->concept_id, ti->title, ti->now, ti->idx, ti->now, ti->tid,
    ti->title, ti->now, ti->now, ti->now, ti->deeplink, ti->huburi,
    ti->meta_path, ti->icon0info, pic);
  return run_sql(db, sql);
}

static int insert_conceptmetadata(sqlite3 *db, const title_info *ti) {
  const char *pic = ti->has_pic0 ? ti->pic0info : NULL;
  char *sql = sqlite3_mprintf(
    "INSERT OR REPLACE INTO tbl_conceptmetadata "
    "(localConceptId,conceptId,conceptName,metaDataPath,lastAccessTime,"
    "lastAccessIndex,sortPriority,pathInfo,pathInfo2,dispLocation,totalSize,"
    "hddLocation,externalHddAppStatus,contentLocation,installStatus,"
    "pprDeeplinkUri,pprHubAppUri,icon0Info,pic0Info,minimumAge,spaceType,"
    "scpDataStored,mergedOperationCapability,mergedContentsStatus,"
    "mergedPromoteTime,mergedInstallTime,mergedAppInfoContentType) "
    "VALUES (%Q,%lld,%Q,%Q,%Q,%lld,100,0,0,138,%lld,1,1,1,66049,%Q,%Q,%Q,%Q,0,0,0,"
    "2,1,%Q,%Q,2)",
    ti->local_concept, ti->concept_id, ti->title, ti->meta_path, ti->now, ti->idx, ti->size,
    ti->deeplink, ti->huburi, ti->icon0info, pic, ti->now, ti->now);
  return run_sql(db, sql);
}


/* ---------------------------------------------------- appinfo.db (launch) */
/* The system keeps a parallel key/value registry in appinfo.db that the
   launcher reads. An app.db row alone shows the tile but won't boot, so we
   mirror the same fields here. */

static void cinfo_i(sqlite3 *adb, const char *lc, const char *k, long long v) {
  run_sql(adb, sqlite3_mprintf(
    "INSERT OR REPLACE INTO tbl_conceptinfo(local_concept_id,key,val) "
    "VALUES(%Q,%Q,%lld)", lc, k, v));
}
static void cinfo_s(sqlite3 *adb, const char *lc, const char *k, const char *v) {
  run_sql(adb, sqlite3_mprintf(
    "INSERT OR REPLACE INTO tbl_conceptinfo(local_concept_id,key,val) "
    "VALUES(%Q,%Q,%Q)", lc, k, v));
}

static void insert_conceptinfo_db(sqlite3 *adb, const title_info *ti) {
  const char *lc = ti->local_concept;
  long long hdd = (ti->storage == STORE_M2) ? 1 : 0;
  cinfo_i(adb, lc, "concept_id", ti->concept_id);
  cinfo_s(adb, lc, "concept_name", ti->title);
  cinfo_i(adb, lc, "content_type", 2);
  cinfo_i(adb, lc, "contents_location", 1);
  cinfo_i(adb, lc, "contents_status", 1);
  cinfo_s(adb, lc, "deeplink_uri", ti->deeplink);
  cinfo_i(adb, lc, "display_location", 138);
  cinfo_i(adb, lc, "external_hdd_app_status", 1);
  cinfo_i(adb, lc, "hdd_location", hdd);
  cinfo_s(adb, lc, "hubapp_uri", ti->huburi);
  cinfo_i(adb, lc, "install_status", 66049);
  cinfo_s(adb, lc, "install_time", ti->now);
  cinfo_s(adb, lc, "last_access_date", ti->now);
  cinfo_i(adb, lc, "last_access_index", ti->idx);
  cinfo_s(adb, lc, "metadata_path", ti->meta_path);
  cinfo_i(adb, lc, "operation_capability", 2);
  cinfo_i(adb, lc, "path_icon0_info", PATH_ICON0_MAGIC);
  cinfo_i(adb, lc, "path_icon0_info_time_stamp", ti->icon_ts);
  cinfo_i(adb, lc, "path_info", 0);
  cinfo_i(adb, lc, "path_info_2", 0);
  cinfo_i(adb, lc, "path_pic0_info", PATH_PIC0_MAGIC);
  cinfo_i(adb, lc, "path_pic0_info_time_stamp", ti->pic_ts);
  cinfo_s(adb, lc, "primary_title_id", ti->tid);
  cinfo_s(adb, lc, "promote_time", ti->now);
  cinfo_i(adb, lc, "sort_priority", 100);
  cinfo_i(adb, lc, "sync_index", ti->idx);
  cinfo_i(adb, lc, "total_size", ti->size);
}

typedef struct { sqlite3 *db; const char *tid; const char *mdid; } ainfo_ctx;

static void ainfo_emit_int(void *c, const char *k, long long v) {
  ainfo_ctx *x = c;
  run_sql(x->db, sqlite3_mprintf(
    "INSERT OR REPLACE INTO tbl_appinfo(titleId,metaDataId,key,val) "
    "VALUES(%Q,%Q,%Q,%lld)", x->tid, x->mdid, k, v));
}
static void ainfo_emit_str(void *c, const char *k, const char *v) {
  ainfo_ctx *x = c;
  run_sql(x->db, sqlite3_mprintf(
    "INSERT OR REPLACE INTO tbl_appinfo(titleId,metaDataId,key,val) "
    "VALUES(%Q,%Q,%Q,%Q)", x->tid, x->mdid, k, v));
}

/* Populate appinfo.db (tbl_appinfo + tbl_conceptinfo) for one title. */
static void insert_appinfo_db(const title_info *ti) {
  sqlite3 *adb = NULL;
  if(sqlite3_open_v2(APPINFO_DB_PATH, &adb,
                     SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
    fprintf(stderr, "fpkg_db: cannot open appinfo.db RW\n");
    if(adb) sqlite3_close(adb);
    return;
  }
  sqlite3_busy_timeout(adb, 3000);
  char mdid[48]; metadata_id_str(ti, mdid, sizeof(mdid));
  /* Drop any stale metaDataId set left from a previous storage location (e.g. a
     "prior:external:<id>" set after the game was moved to internal). Otherwise
     the launcher may resolve the wrong, now-bogus path. Keep only the set we're
     about to (re)write. */
  run_sql(adb, sqlite3_mprintf(
    "DELETE FROM tbl_appinfo WHERE titleId=%Q AND metaDataId<>%Q", ti->tid, mdid));
  ainfo_ctx x = { adb, ti->tid, mdid };
  field_sink s = { ainfo_emit_int, ainfo_emit_str, &x };
  emit_fields(ti, &s);
  insert_conceptinfo_db(adb, ti);
  sqlite3_close(adb);
}


/* -------------------------------------------------------------- driver */

static int title_id_valid(const char *n) {
  size_t l = strlen(n);
  if(l < 8 || l > 12) return 0;
  for(size_t i = 0; i < l; i++)
    if(!((n[i]>='A'&&n[i]<='Z')||(n[i]>='0'&&n[i]<='9'))) return 0;
  return 1;
}

static long long next_index(sqlite3 *db) {
  sqlite3_stmt *st = NULL;
  long long idx = 1;
  if(sqlite3_prepare_v2(db, "SELECT MAX(lastAccessIndex) FROM tbl_contentinfo",
                        -1, &st, NULL) == SQLITE_OK) {
    if(sqlite3_step(st) == SQLITE_ROW) idx = sqlite3_column_int64(st, 0) + 1;
  }
  sqlite3_finalize(st);
  return idx;
}

/* Collect per-user table suffixes (the digits after tbl_iconinfo_). */
#define MAX_USERS 8
static int collect_user_suffixes(sqlite3 *db, char suffixes[][16]) {
  sqlite3_stmt *st = NULL;
  int n = 0;
  if(sqlite3_prepare_v2(db,
        "SELECT substr(name,14) FROM sqlite_master WHERE type='table' "
        "AND name LIKE 'tbl_iconinfo_%'", -1, &st, NULL) == SQLITE_OK) {
    while(n < MAX_USERS && sqlite3_step(st) == SQLITE_ROW) {
      const char *s = (const char*)sqlite3_column_text(st, 0);
      if(s) { snprintf(suffixes[n], 16, "%s", s); n++; }
    }
  }
  sqlite3_finalize(st);
  return n;
}

static int add_one_title(sqlite3 *db, const char *tid, int storage,
                         const char *app_root, char suffixes[][16],
                         int nusers, long long idx) {
  title_info ti;
  if(load_title_info(tid, idx, storage, app_root, &ti) != 0) {
    fprintf(stderr, "fpkg_db: %s — no readable param.sfo, skipping\n", tid);
    return -1;
  }
  if(insert_contentinfo(db, &ti) != 0) return -1;
  insert_conceptmetadata(db, &ti);
  for(int u = 0; u < nusers; u++) {
    insert_iconinfo(db, suffixes[u], &ti);
    insert_concepticoninfo(db, suffixes[u], &ti);
  }
  /* The launcher reads appinfo.db, not app.db — mirror the title there too,
     otherwise the tile shows but "can't start". */
  insert_appinfo_db(&ti);
  fprintf(stderr, "fpkg_db: added %s (%s) [%s]\n", tid, ti.title,
          storage == STORE_M2 ? "M.2" : "internal");
  return 0;
}

/* Restore one title's complete launchable rows verbatim from the pre-rebuild
   backup DBs (app.db on the already-open `db` handle via ATTACH; appinfo.db on
   its own handle). Returns 0 if restored, -1 if the title isn't in the backup
   (caller then synthesises). */
static int restore_title(sqlite3 *db, const char *tid,
                         char suffixes[][16], int nusers) {
  if(file_size(g_bk_app) <= 0 || file_size(g_bk_appinfo) <= 0) return -1;

  char *att = sqlite3_mprintf("ATTACH %Q AS bk", g_bk_app);
  int arc = sqlite3_exec(db, att, NULL, NULL, NULL);
  sqlite3_free(att);
  if(arc != SQLITE_OK) return -1;

  /* Pull the title's concept from the backup (also confirms it's present). */
  char concept[64] = "";
  sqlite3_stmt *st = NULL;
  if(sqlite3_prepare_v2(db, "SELECT localConceptId FROM bk.tbl_contentinfo "
                            "WHERE titleId=?1 LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(st) == SQLITE_ROW) {
      const char *c = (const char*)sqlite3_column_text(st, 0);
      if(c) snprintf(concept, sizeof(concept), "%s", c);
    }
  }
  sqlite3_finalize(st);
  if(!concept[0]) { sqlite3_exec(db, "DETACH bk", NULL, NULL, NULL); return -1; }

  run_sql(db, sqlite3_mprintf(
    "INSERT OR REPLACE INTO tbl_contentinfo SELECT * FROM bk.tbl_contentinfo "
    "WHERE titleId=%Q", tid));
  run_sql(db, sqlite3_mprintf(
    "INSERT OR REPLACE INTO tbl_conceptmetadata SELECT * FROM bk.tbl_conceptmetadata "
    "WHERE localConceptId=%Q", concept));
  for(int u = 0; u < nusers; u++) {
    run_sql(db, sqlite3_mprintf(
      "INSERT OR REPLACE INTO tbl_iconinfo_%s SELECT * FROM bk.tbl_iconinfo_%s "
      "WHERE titleId=%Q", suffixes[u], suffixes[u], tid));
    run_sql(db, sqlite3_mprintf(
      "INSERT OR REPLACE INTO tbl_concepticoninfo_%s SELECT * FROM bk.tbl_concepticoninfo_%s "
      "WHERE localConceptId=%Q", suffixes[u], suffixes[u], concept));
  }
  sqlite3_exec(db, "DETACH bk", NULL, NULL, NULL);

  /* appinfo.db on its own handle (the live app.db handle is busy). */
  sqlite3 *adb = NULL;
  if(sqlite3_open_v2(APPINFO_DB_PATH, &adb,
                     SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) == SQLITE_OK) {
    sqlite3_busy_timeout(adb, 3000);
    if(sqlite3_exec(adb, sqlite3_mprintf("ATTACH %Q AS bk", g_bk_appinfo),
                    NULL, NULL, NULL) == SQLITE_OK) {
      run_sql(adb, sqlite3_mprintf(
        "INSERT OR REPLACE INTO tbl_appinfo SELECT * FROM bk.tbl_appinfo "
        "WHERE titleId=%Q", tid));
      run_sql(adb, sqlite3_mprintf(
        "INSERT OR REPLACE INTO tbl_conceptinfo SELECT * FROM bk.tbl_conceptinfo "
        "WHERE local_concept_id=%Q", concept));
      sqlite3_exec(adb, "DETACH bk", NULL, NULL, NULL);
    }
    sqlite3_close(adb);
  }
  fprintf(stderr, "fpkg_db: RESTORED %s from backup (concept %s)\n", tid, concept);
  return 0;
}

/* Scan one app dir. For each CUSA title whose live registration is missing or
   stale (points at a path with no pkg), re-establish it FROM THE TITLE'S ACTUAL
   CURRENT LOCATION: synthesise internal/M.2 rows, restore USB from backup. A
   title whose registration already matches a real pkg is left untouched. Dead
   stubs (no data anywhere) are skipped. */
static void scan_app_dir(sqlite3 *db, const char *dir, char suffixes[][16],
                         int nusers, int *added,
                         char handled[][16], int *nhandled) {
  DIR *d = opendir(dir);
  if(!d) return;
  struct dirent *e;
  while((e = readdir(d))) {
    if(e->d_name[0] == '.')                  continue;
    if(!title_id_valid(e->d_name))           continue;
    /* Only PS4 fpkg titles (CUSA) — PS5 (PPSA) are handled by the system. */
    if(strncmp(e->d_name, "CUSA", 4) != 0)   continue;

    /* A title can appear in both /user/app (stub) and the M.2 app dir — only
       handle each once across the two scans. */
    int seen = 0;
    for(int h = 0; h < *nhandled; h++)
      if(!strcmp(handled[h], e->d_name)) { seen = 1; break; }
    if(seen) continue;

    char app_root[64];
    int storage = detect_storage(e->d_name, app_root, sizeof(app_root));
    if(storage == STORE_NONE) continue;   /* dead stub — no data anywhere */

    if(*nhandled < 64) snprintf(handled[(*nhandled)++], 16, "%s", e->d_name);

    /* For USB: if the live registration already points at a real app.pkg, trust
       it — we can't synthesise the per-device hddLocation anyway. For internal
       and M.2 we always re-synthesise so fields like installStatus and
       packageLocation stay correct even if a prior run wrote stale values. */
    char regpath[96];
    reg_org_path(db, e->d_name, regpath, sizeof(regpath));
    if(regpath[0] && storage == STORE_USB) {
      char regpkg[160];
      snprintf(regpkg, sizeof(regpkg), "%s/app.pkg", regpath);
      if(file_size(regpkg) > 0) continue;   /* USB registration valid — leave it */
    }

    /* Registration is missing (wiped by a DB rebuild) or stale (points to a path
       with no pkg — the title was moved while our 0555 lock blocked the move's
       registration rewrite). Re-establish it from where the data ACTUALLY is. */
    if(storage == STORE_USB) {
      /* USB needs the per-device id we can't synthesise — restore the verbatim
         row from the backup if present, else leave it for the system's own
         mount-time re-registration. */
      if(restore_title(db, e->d_name, suffixes, nusers) == 0) (*added)++;
      continue;
    }
    /* Internal / M.2: synthesise a storage-accurate row from the real location
       (carries the real offline concept). add_one_title() also clears any stale
       metaDataId set left over in appinfo.db. */
    long long idx = next_index(db);
    if(add_one_title(db, e->d_name, storage, app_root,
                     suffixes, nusers, idx) == 0)
      (*added)++;
  }
  closedir(d);
}

/* ---------------------------------------------------- read-only protection */
/* Marking the on-disk content + license files read-only (0555) stops a Sony
   DB rebuild / GC from deleting them, so games on extended storage (and their
   fake licenses) survive. */


/* Recursively chmod every file and directory under `path` to `mode`.
   Used by the unlock path so nothing inside a CUSA folder stays restricted. */
static void chmod_tree(const char *path, int mode) {
  chmod(path, mode);
  DIR *d = opendir(path);
  if(!d) return;
  struct dirent *e;
  while((e = readdir(d))) {
    if(!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
    char p[576];
    snprintf(p, sizeof p, "%s/%s", path, e->d_name);
    struct stat st;
    if(stat(p, &st) != 0) continue;
    if(S_ISDIR(st.st_mode)) chmod_tree(p, mode);
    else chmod(p, mode);
  }
  closedir(d);
}

/* Lock (lock=1): set the CUSA folder itself to 0555. The parent-dir permission
   is what blocks unlink/rename, so locking only the root is sufficient; all
   files inside stay 0777 so the game launches and subdir contents are readable.
   Unlock (lock=0): recursively set everything to 0777 so nothing is left
   restricted after the user needs to install, move, or delete. */
static void apply_cusa_dir(const char *appdir, int lock) {
  if(lock) {
    /* Lock: files/subdirs stay 0777; only the folder root goes 0555. */
    chmod(appdir, 0555);
  } else {
    /* Unlock: full recursive 0777 — leave no locked file or subdir behind. */
    chmod_tree(appdir, 0777);
  }
}

/* Apply to every CUSA app folder under `root`. */
static void apply_app_root(const char *root, int lock) {
  DIR *d = opendir(root);
  if(!d) return;
  struct dirent *e;
  while((e = readdir(d))) {
    if(strncmp(e->d_name, "CUSA", 4) != 0) continue;
    char appdir[320];
    snprintf(appdir, sizeof(appdir), "%s/%s", root, e->d_name);
    struct stat st;
    if(stat(appdir, &st) == 0 && S_ISDIR(st.st_mode))
      apply_cusa_dir(appdir, lock);
  }
  closedir(d);
}

/* Apply to CUSA-named license files (.rif/.idx) in a license directory. */
static void apply_license_root(const char *root, int lock) {
  DIR *d = opendir(root);
  if(!d) return;
  struct dirent *e;
  while((e = readdir(d))) {
    if(strncmp(e->d_name, "CUSA", 4) != 0) continue;
    char p[400];
    snprintf(p, sizeof p, "%s/%s", root, e->d_name);
    chmod(p, lock ? 0555 : 0777);
  }
  closedir(d);
}

/* Lock / unlock all three drives + license files + DLC/patch/playgo. */
static void apply_all(int lock) {
  apply_app_root(APP_DIR,      lock);
  apply_app_root(EXT0_APP_DIR, lock);
  apply_app_root(EXT1_APP_DIR, lock);
  apply_license_root("/user/license",            lock);
  apply_license_root("/mnt/ext0/user/license",   lock);
  apply_license_root("/mnt/ext1/user/license",   lock);
  apply_app_root(ADDCONT_DIR,      lock);
  apply_app_root(EXT0_ADDCONT_DIR, lock);
  apply_app_root(EXT1_ADDCONT_DIR, lock);
  apply_app_root(PATCH_DIR,        lock);
  apply_app_root(EXT0_PATCH_DIR,   lock);
  apply_app_root(EXT1_PATCH_DIR,   lock);
  apply_app_root(PLAYGO_DIR,       lock);
  apply_app_root(EXT0_PLAYGO_DIR,  lock);
  apply_app_root(EXT1_PLAYGO_DIR,  lock);
}

/* Lock: all CUSA folders -> 0555. Undeletable yet launchable. */
void fpkg_protect_files(void)   { apply_all(1); }

/* Release: all CUSA folders -> 0777 so games can be moved/deleted again. */
void fpkg_unprotect_files(void) { apply_all(0); }

/* Returns 1 if at least one CUSA folder on any drive is currently locked. */
int fpkg_is_protected(void) {
  const char *roots[] = { APP_DIR, EXT0_APP_DIR, EXT1_APP_DIR, NULL };
  for(int r = 0; roots[r]; r++) {
    DIR *d = opendir(roots[r]);
    if(!d) continue;
    struct dirent *e;
    while((e = readdir(d))) {
      if(strncmp(e->d_name, "CUSA", 4) != 0) continue;
      char p[320];
      snprintf(p, sizeof(p), "%s/%s", roots[r], e->d_name);
      struct stat st;
      if(stat(p, &st) == 0 && (st.st_mode & 0777) == 0555) {
        closedir(d);
        return 1;
      }
    }
    closedir(d);
  }
  return 0;
}

/* True while a title move is in progress — the system stages the copy in a
   "temp/move" directory that holds a CUSA* entry until the move finishes. The
   daemon uses this to auto-unlock during a move (so our 0555 lock doesn't block
   the delete-source step) and re-lock once it's done. */
/* Check one directory for any entry whose name starts with CUSA. */
static int dir_has_cusa(const char *path) {
  DIR *d = opendir(path);
  if(!d) return 0;
  int found = 0;
  struct dirent *e;
  while((e = readdir(d))) {
    if(strncmp(e->d_name, "CUSA", 4) == 0) { found = 1; break; }
  }
  closedir(d);
  return found;
}

/* Check a root app dir for any hidden (dot-prefixed) sub-directory that
   contains a CUSA entry — these are Sony's in-progress move staging dirs. */
static int app_root_has_move_staging(const char *root) {
  DIR *d = opendir(root);
  if(!d) return 0;
  int found = 0;
  struct dirent *e;
  while((e = readdir(d)) && !found) {
    if(e->d_name[0] != '.') continue;
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char sub[320];
    snprintf(sub, sizeof sub, "%s/%s", root, e->d_name);
    struct stat st;
    if(stat(sub, &st) == 0 && S_ISDIR(st.st_mode) && dir_has_cusa(sub))
      found = 1;
  }
  closedir(d);
  return found;
}

int fpkg_move_in_progress(void) {
  /* Sony's known temp-move locations (vary by FW). */
  const char *known[] = {
    "/user/temp/move/app",
    "/mnt/ext0/user/temp/move/app", "/mnt/ext0/ps5/temp/app",
    "/mnt/ext1/user/temp/move/app", "/mnt/ext1/ps5/temp/app",
    NULL
  };
  for(int i = 0; known[i]; i++) {
    if(dir_has_cusa(known[i])) return 1;
  }
  /* Fallback: any hidden staging dir directly under an app root. */
  const char *app_roots[] = { APP_DIR, EXT0_APP_DIR, EXT1_APP_DIR, NULL };
  for(int i = 0; app_roots[i]; i++) {
    if(app_root_has_move_staging(app_roots[i])) return 1;
  }
  return 0;
}

/* Proactive install-pending check: scan USB drives for .pkg files and see if
   the CUSA title they contain has a locked (0555) app folder on any drive.
   This fires BEFORE Sony's installer even attempts to create a BGFT task, so
   we can unlock proactively and avoid the first-attempt failure entirely.
   Returns 1 if any pending-install condition is found, 0 otherwise. */
int fpkg_usb_install_pending(void) {
  const char *usb_dirs[] = { "/mnt/usb0", "/mnt/usb1", NULL };
  const char *app_roots[] = { APP_DIR, EXT0_APP_DIR, EXT1_APP_DIR, NULL };
  for(int u = 0; usb_dirs[u]; u++) {
    DIR *d = opendir(usb_dirs[u]);
    if(!d) continue;
    struct dirent *e;
    while((e = readdir(d))) {
      size_t len = strlen(e->d_name);
      if(len < 5) continue;
      const char *ext = e->d_name + len - 4;
      if(!(ext[0]=='.' &&
           (ext[1]=='p'||ext[1]=='P') &&
           (ext[2]=='k'||ext[2]=='K') &&
           (ext[3]=='g'||ext[3]=='G'))) continue;
      char pkg_path[600];
      snprintf(pkg_path, sizeof(pkg_path), "%s/%s", usb_dirs[u], e->d_name);
      FILE *fp = fopen(pkg_path, "rb");
      if(!fp) continue;
      char buf[512];
      size_t n = fread(buf, 1, sizeof(buf), fp);
      fclose(fp);
      /* Extract the CUSA title id from the pkg header. */
      char tid[16] = "";
      for(size_t i = 0; i + 4 <= n && !tid[0]; i++) {
        if(buf[i]!='C'||buf[i+1]!='U'||buf[i+2]!='S'||buf[i+3]!='A') continue;
        size_t j;
        for(j = 0; j < 12 && i+j < n; j++) {
          char c = buf[i+j];
          if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9'))) break;
          tid[j] = c;
        }
        tid[j] = '\0';
        if(j < 8) tid[0] = '\0';   /* too short — not a real title id */
      }
      if(!tid[0]) continue;
      for(int a = 0; app_roots[a]; a++) {
        char appdir[400];
        snprintf(appdir, sizeof(appdir), "%s/%s", app_roots[a], tid);
        struct stat st;
        if(stat(appdir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if((st.st_mode & 0777) == 0555) {
          closedir(d);
          return 1;
        }
      }
    }
    closedir(d);
  }
  return 0;
}

/* True while a CUSA pkg install (or overwrite) is in progress — detected by
   watching for recently-modified BGFT task records that reference a CUSA title,
   OR proactively via a USB .pkg that matches a locked CUSA folder.
   The daemon uses this to auto-unlock before the installer tries to overwrite
   the locked files, then re-lock once the install finishes. */
int fpkg_install_in_progress(void) {
  /* Proactive: pkg on USB targets a locked CUSA folder → unlock now, before
     the first install attempt even fires. */
  if(fpkg_usb_install_pending()) return 1;

  const char *bgft_roots[] = {
    "/user/bgft/task",
    "/user/bgft/ext0/task",
    NULL
  };
  time_t now = time(NULL);
  for(int r = 0; bgft_roots[r]; r++) {
    DIR *d = opendir(bgft_roots[r]);
    if(!d) continue;
    struct dirent *e;
    while((e = readdir(d))) {
      if(e->d_name[0] == '.') continue;
      char pdb[200];
      snprintf(pdb, sizeof(pdb), "%s/%s/d0.pdb", bgft_roots[r], e->d_name);
      struct stat st;
      if(stat(pdb, &st) != 0) continue;
      if(now - st.st_mtime > 15) continue;  /* only recently active tasks */
      FILE *fp = fopen(pdb, "rb");
      if(!fp) continue;
      char buf[1024];
      size_t n = fread(buf, 1, sizeof(buf), fp);
      fclose(fp);
      /* Manual search — memmem not available on this SDK. */
      int cusa_found = 0;
      for(size_t i = 0; i + 4 <= n; i++) {
        if(buf[i]=='C' && buf[i+1]=='U' && buf[i+2]=='S' && buf[i+3]=='A') {
          cusa_found = 1; break;
        }
      }
      if(cusa_found) {
        closedir(d);
        return 1;
      }
    }
    closedir(d);
  }
  return 0;
}

/* Lock one title's app, addcont, patch, and playgo folders on all drives. */
static void protect_title(const char *tid) {
  const char *roots[] = {
    APP_DIR,      EXT0_APP_DIR,      EXT1_APP_DIR,
    ADDCONT_DIR,  EXT0_ADDCONT_DIR,  EXT1_ADDCONT_DIR,
    PATCH_DIR,    EXT0_PATCH_DIR,    EXT1_PATCH_DIR,
    PLAYGO_DIR,   EXT0_PLAYGO_DIR,   EXT1_PLAYGO_DIR,
    NULL
  };
  for(int i = 0; roots[i]; i++) {
    char appdir[320];
    snprintf(appdir, sizeof(appdir), "%s/%s", roots[i], tid);
    struct stat st;
    if(stat(appdir, &st) == 0 && S_ISDIR(st.st_mode))
      apply_cusa_dir(appdir, 1);
  }
}

/* ---------------------------------------------------- orphan move-source sweep */
/* A move copies the game to the destination drive and the system writes a fresh
   registration pointing there — but our 0555 lock blocks the move's delete of
   the SOURCE copy, leaving a stale duplicate on the old drive. On the next mount
   the system can re-import that orphan and clobber the good registration. This
   sweep removes such orphans: a CUSA app folder that is NOT the title's
   registered location, when the registered location holds a real pkg. */

/* Set to 0 to actually delete; 1 logs the targets only (verify first, on-device,
   that it lists ONLY true orphans before enabling deletion). */
#ifndef FPKG_SWEEP_DRY_RUN
#define FPKG_SWEEP_DRY_RUN 1
#endif

/* Recursively delete a directory tree (chmod 0777 first so a 0555-locked folder
   can be emptied). Only called when the sweep deletes for real (not dry-run). */
__attribute__((unused)) static void rmrf(const char *path) {
  DIR *d = opendir(path);
  if(d) {
    struct dirent *e;
    while((e = readdir(d))) {
      if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      char p[1024];
      snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
      struct stat st;
      if(stat(p, &st) == 0 && S_ISDIR(st.st_mode)) rmrf(p);
      else { chmod(p, 0777); unlink(p); }
    }
    closedir(d);
  }
  chmod(path, 0777);
  rmdir(path);
}

int fpkg_sweep_orphans(void) {
  int removed = 0;
  sqlite3 *db = NULL;
  if(sqlite3_open_v2(APP_DB_PATH, &db,
                     SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
    if(db) sqlite3_close(db);
    return 0;
  }
  sqlite3_busy_timeout(db, 3000);

  struct { const char *root; const char *priv; } drives[] = {
    { APP_DIR,      "/system_data/priv/appmeta" },
    { EXT0_APP_DIR, "/mnt/ext0/system_data/priv/appmeta" },
    { EXT1_APP_DIR, "/mnt/ext1/system_data/priv/appmeta" },
    { NULL, NULL }
  };
  for(int r = 0; drives[r].root; r++) {
    DIR *d = opendir(drives[r].root);
    if(!d) continue;
    struct dirent *e;
    while((e = readdir(d))) {
      if(strncmp(e->d_name, "CUSA", 4) != 0) continue;
      char here[160], herepkg[200];
      snprintf(here,    sizeof(here),    "%s/%s", drives[r].root, e->d_name);
      snprintf(herepkg, sizeof(herepkg), "%s/app.pkg", here);
      if(file_size(herepkg) <= 0) continue;          /* no real copy here */

      char regpath[96];
      reg_org_path(db, e->d_name, regpath, sizeof(regpath));
      if(!regpath[0]) continue;                       /* unregistered: repair handles it */
      if(strcmp(regpath, here) == 0) continue;        /* this IS the canonical copy */
      char regpkg[160];
      snprintf(regpkg, sizeof(regpkg), "%s/app.pkg", regpath);
      if(file_size(regpkg) <= 0) continue;            /* registration also stale: repair first */

      /* here != registered location, and the registered location has a real pkg
         -> `here` is a leftover move-source orphan. */
      fprintf(stderr, "fpkg_db: ORPHAN move-source %s (canonical=%s)%s\n",
              here, regpath, FPKG_SWEEP_DRY_RUN ? " [dry-run]" : "");
#if !FPKG_SWEEP_DRY_RUN
      apply_cusa_dir(here, 0);                         /* unlock to allow delete */
      rmrf(here);
      /* on-drive external appmeta for this orphan (never touch internal's shared
         /system_data appmeta tree, which feeds synth's param.sfo reads). */
      if(drives[r].root != APP_DIR) {
        char meta[200];
        snprintf(meta, sizeof(meta), "%s/%s", drives[r].priv, e->d_name);
        if(file_exists(meta)) rmrf(meta);
      }
#endif
      removed++;
    }
    closedir(d);
  }
  sqlite3_close(db);
  if(removed)
    fprintf(stderr, "fpkg_db: orphan sweep %s %d folder(s)\n",
            FPKG_SWEEP_DRY_RUN ? "found (dry-run)" : "removed", removed);
  return removed;
}


int fpkg_db_repair_src(int use_prev, int *out_added) {
  if(out_added) *out_added = 0;

  /* Point the restore at the requested backup set for this run. */
  snprintf(g_bk_app, sizeof(g_bk_app), "%s",
           use_prev ? BACKUP_PREV_APP_DB : BACKUP_CUR_APP_DB);
  snprintf(g_bk_appinfo, sizeof(g_bk_appinfo), "%s",
           use_prev ? BACKUP_PREV_APPINFO_DB : BACKUP_CUR_APPINFO_DB);

  sqlite3 *db = NULL;
  if(sqlite3_open_v2(APP_DB_PATH, &db,
                     SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
    fprintf(stderr, "fpkg_db: cannot open %s RW (kstuff loaded?)\n", APP_DB_PATH);
    if(db) sqlite3_close(db);
    return -1;
  }
  sqlite3_busy_timeout(db, 3000);

  char suffixes[MAX_USERS][16];
  int nusers = collect_user_suffixes(db, suffixes);

  /* Scan the internal app dir (stub dirs survive a rebuild) AND the M.2 app
     dir directly, in case a title has no internal stub. Already-added titles
     are skipped on the second pass, so the union is deduped naturally. */
  int added = 0;
  char handled[64][16];
  int  nhandled = 0;
  scan_app_dir(db, APP_DIR,      suffixes, nusers, &added, handled, &nhandled);
  scan_app_dir(db, EXT1_APP_DIR, suffixes, nusers, &added, handled, &nhandled);
  scan_app_dir(db, EXT0_APP_DIR, suffixes, nusers, &added, handled, &nhandled);

  sqlite3_close(db);
  if(out_added) *out_added = added;
  return 0;
}

int fpkg_db_repair(int *out_added) {
  return fpkg_db_repair_src(0, out_added);   /* default: the "current" backup */
}


/* ----------------------------------------------------------- backup sync */

/* Safe whole-db copy via sqlite's online backup API (consistent even while
   the system has the source open). Used once to seed the backup schema. */
static int db_online_copy(const char *src, const char *dst) {
  sqlite3 *s = NULL, *d = NULL;
  int rc = -1;
  if(sqlite3_open_v2(src, &s, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK)
    goto out;
  if(sqlite3_open_v2(dst, &d, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                     SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK)
    goto out;
  sqlite3_busy_timeout(s, 3000);
  sqlite3_busy_timeout(d, 3000);
  sqlite3_backup *b = sqlite3_backup_init(d, "main", s, "main");
  if(b) {
    sqlite3_backup_step(b, -1);
    if(sqlite3_backup_finish(b) == SQLITE_OK) rc = 0;
  }
out:
  if(s) sqlite3_close(s);
  if(d) sqlite3_close(d);
  return rc;
}

/* Collect the live CUSA titles NOT yet in the current backup (= fresh installs
   since the last sync) into `out`; returns the count. If the backup is missing,
   all are new. */
static int collect_new_installs(char tids[][16], int n, char out[][16]) {
  int nnew = 0;
  if(file_size(BACKUP_CUR_APP_DB) <= 0) {
    for(int i = 0; i < n && nnew < 64; i++) snprintf(out[nnew++], 16, "%s", tids[i]);
    return nnew;
  }
  sqlite3 *cb = NULL;
  if(sqlite3_open_v2(BACKUP_CUR_APP_DB, &cb,
                     SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
    if(cb) sqlite3_close(cb);
    return 0;
  }
  sqlite3_stmt *st = NULL;
  if(sqlite3_prepare_v2(cb, "SELECT 1 FROM tbl_contentinfo WHERE titleId=?1 LIMIT 1",
                        -1, &st, NULL) == SQLITE_OK) {
    for(int i = 0; i < n && nnew < 64; i++) {
      sqlite3_reset(st);
      sqlite3_bind_text(st, 1, tids[i], -1, SQLITE_TRANSIENT);
      if(sqlite3_step(st) != SQLITE_ROW) snprintf(out[nnew++], 16, "%s", tids[i]);
    }
  }
  sqlite3_finalize(st);
  sqlite3_close(cb);
  return nnew;
}

/* Copy each given CUSA title's rows from the live DBs into the "current"
   backup set (per-title INSERT OR REPLACE; never removes). */
static void accumulate_current(char tids[][16], int n) {
  sqlite3 *bk = NULL;
  if(sqlite3_open_v2(BACKUP_CUR_APP_DB, &bk,
                     SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) == SQLITE_OK) {
    sqlite3_busy_timeout(bk, 3000);
    if(sqlite3_exec(bk, sqlite3_mprintf("ATTACH %Q AS lv", APP_DB_PATH),
                    NULL, NULL, NULL) == SQLITE_OK) {
      char sfx[MAX_USERS][16];
      int nu = collect_user_suffixes(bk, sfx);
      for(int i = 0; i < n; i++) {
        char concept[64] = "";
        sqlite3_stmt *c = NULL;
        if(sqlite3_prepare_v2(bk, "SELECT localConceptId FROM lv.tbl_contentinfo "
                                  "WHERE titleId=?1 LIMIT 1", -1, &c, NULL) == SQLITE_OK) {
          sqlite3_bind_text(c, 1, tids[i], -1, SQLITE_TRANSIENT);
          if(sqlite3_step(c) == SQLITE_ROW) {
            const char *cc = (const char*)sqlite3_column_text(c, 0);
            if(cc) snprintf(concept, sizeof(concept), "%s", cc);
          }
        }
        sqlite3_finalize(c);
        run_sql(bk, sqlite3_mprintf(
          "INSERT OR REPLACE INTO tbl_contentinfo SELECT * FROM lv.tbl_contentinfo "
          "WHERE titleId=%Q", tids[i]));
        if(concept[0])
          run_sql(bk, sqlite3_mprintf(
            "INSERT OR REPLACE INTO tbl_conceptmetadata SELECT * FROM lv.tbl_conceptmetadata "
            "WHERE localConceptId=%Q", concept));
        for(int u = 0; u < nu; u++) {
          run_sql(bk, sqlite3_mprintf(
            "INSERT OR REPLACE INTO tbl_iconinfo_%s SELECT * FROM lv.tbl_iconinfo_%s "
            "WHERE titleId=%Q", sfx[u], sfx[u], tids[i]));
          if(concept[0])
            run_sql(bk, sqlite3_mprintf(
              "INSERT OR REPLACE INTO tbl_concepticoninfo_%s SELECT * FROM lv.tbl_concepticoninfo_%s "
              "WHERE localConceptId=%Q", sfx[u], sfx[u], concept));
        }
      }
      sqlite3_exec(bk, "DETACH lv", NULL, NULL, NULL);
    }
    sqlite3_close(bk);
  }

  sqlite3 *bki = NULL;
  if(sqlite3_open_v2(BACKUP_CUR_APPINFO_DB, &bki,
                     SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) == SQLITE_OK) {
    sqlite3_busy_timeout(bki, 3000);
    if(sqlite3_exec(bki, sqlite3_mprintf("ATTACH %Q AS lv", APPINFO_DB_PATH),
                    NULL, NULL, NULL) == SQLITE_OK) {
      for(int i = 0; i < n; i++) {
        char concept[64] = "";
        sqlite3_stmt *c = NULL;
        if(sqlite3_prepare_v2(bki, "SELECT val FROM lv.tbl_appinfo "
                                   "WHERE titleId=?1 AND key='_local_concept_id' LIMIT 1",
                                   -1, &c, NULL) == SQLITE_OK) {
          sqlite3_bind_text(c, 1, tids[i], -1, SQLITE_TRANSIENT);
          if(sqlite3_step(c) == SQLITE_ROW) {
            const char *cc = (const char*)sqlite3_column_text(c, 0);
            if(cc) snprintf(concept, sizeof(concept), "%s", cc);
          }
        }
        sqlite3_finalize(c);
        run_sql(bki, sqlite3_mprintf(
          "INSERT OR REPLACE INTO tbl_appinfo SELECT * FROM lv.tbl_appinfo "
          "WHERE titleId=%Q", tids[i]));
        if(concept[0])
          run_sql(bki, sqlite3_mprintf(
            "INSERT OR REPLACE INTO tbl_conceptinfo SELECT * FROM lv.tbl_conceptinfo "
            "WHERE local_concept_id=%Q", concept));
      }
      sqlite3_exec(bki, "DETACH lv", NULL, NULL, NULL);
    }
    sqlite3_close(bki);
  }
}

/* Snapshot the fpkg registration. "current" is per-title ACCUMULATE (union):
   copy each live CUSA title's rows in, never removing — the system
   auto-restores USB titles post-rebuild, so a whole-DB copy could overwrite
   a good backup with a partial (M.2-missing) state. On EACH new install we
   first snapshot current -> previous (before folding the new title in), so
   "previous" is always the last working state without the newest game — a
   one-install rollback if an install corrupts the live DB.
   Stale entries are harmless: restore filters by on-disk data presence. */
int fpkg_backup_sync(int *out_titles) {
  if(out_titles) *out_titles = 0;
  mkdir(BACKUP_ROOT, 0755);
  mkdir(BACKUP_ROOT "/current", 0755);
  mkdir(BACKUP_ROOT "/previous", 0755);

  /* Collect live CUSA title ids. */
  sqlite3 *lv = NULL;
  if(sqlite3_open_v2(APP_DB_PATH, &lv,
                     SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
    if(lv) sqlite3_close(lv);
    return -1;
  }
  char tids[64][16];
  int n = 0;
  sqlite3_stmt *st = NULL;
  if(sqlite3_prepare_v2(lv, "SELECT titleId FROM tbl_contentinfo "
                            "WHERE titleId LIKE 'CUSA%'", -1, &st, NULL) == SQLITE_OK) {
    while(n < 64 && sqlite3_step(st) == SQLITE_ROW) {
      const char *t = (const char*)sqlite3_column_text(st, 0);
      if(t) snprintf(tids[n++], 16, "%s", t);
    }
  }
  sqlite3_finalize(st);
  sqlite3_close(lv);
  if(n == 0) return 0;   /* nothing live to capture — preserve existing backups */

  /* Which titles are freshly installed (not in the current backup yet). */
  char newt[64][16];
  int  newn = collect_new_installs(tids, n, newt);

  /* On any new install, snapshot the current backup -> previous BEFORE folding
     the new title in. "previous" then holds the last working state without the
     newest game (one-install rollback). */
  if(newn > 0 && file_size(BACKUP_CUR_APP_DB) > 0) {
    db_online_copy(BACKUP_CUR_APP_DB,     BACKUP_PREV_APP_DB);
    db_online_copy(BACKUP_CUR_APPINFO_DB, BACKUP_PREV_APPINFO_DB);
  }

  /* Also take a full-DB snapshot on new installs: rotates current→1installback,
     then captures the live DBs verbatim so restoring gives a working state. */
  if(newn > 0) fpkg_db_full_snapshot();

  /* Seed the current backup schema the first time. */
  if(file_size(BACKUP_CUR_APP_DB) <= 0)     db_online_copy(APP_DB_PATH, BACKUP_CUR_APP_DB);
  if(file_size(BACKUP_CUR_APPINFO_DB) <= 0) db_online_copy(APPINFO_DB_PATH, BACKUP_CUR_APPINFO_DB);

  accumulate_current(tids, n);

  /* Auto-lock freshly installed titles (0555 folder, 0777 files) — everywhere
     they live. Only NEW titles, so a manual Unlock is never silently undone. */
  for(int i = 0; i < newn; i++) protect_title(newt[i]);

  if(out_titles) *out_titles = n;
  return 0;
}


/* ------------------------------------------------------ full DB snapshot */

#define SNAP_ROOT   BACKUP_ROOT "/snap"

int fpkg_db_full_snapshot(void) {
  mkdir(BACKUP_ROOT,          0755);
  mkdir(SNAP_ROOT,            0755);
  mkdir(SNAP_ROOT "/current", 0755);
  mkdir(SNAP_ROOT "/1installback", 0755);

  /* Rotate: current → 1installback before capturing a fresh current. */
  if(file_size(SNAP_ROOT "/current/app.db") > 0) {
    db_online_copy(SNAP_ROOT "/current/app.db",
                   SNAP_ROOT "/1installback/app.db");
    db_online_copy(SNAP_ROOT "/current/appinfo.db",
                   SNAP_ROOT "/1installback/appinfo.db");
  }

  int r = 0;
  if(db_online_copy(APP_DB_PATH,     SNAP_ROOT "/current/app.db")     != 0) r = -1;
  if(db_online_copy(APPINFO_DB_PATH, SNAP_ROOT "/current/appinfo.db") != 0) r = -1;
  fprintf(stderr, "fpkg_db: full DB snapshot %s\n", r == 0 ? "OK" : "FAILED");
  return r;
}

int fpkg_db_full_restore(const char *slot) {
  if(!slot || !*slot) return -1;
  char src_app[160], src_ai[160];
  snprintf(src_app, sizeof(src_app), SNAP_ROOT "/%s/app.db",     slot);
  snprintf(src_ai,  sizeof(src_ai),  SNAP_ROOT "/%s/appinfo.db", slot);
  if(file_size(src_app) <= 0) {
    fprintf(stderr, "fpkg_db: restore: no snapshot at slot '%s'\n", slot);
    return -1;
  }
  int r = 0;
  if(db_online_copy(src_app, APP_DB_PATH)     != 0) r = -1;
  if(db_online_copy(src_ai,  APPINFO_DB_PATH) != 0) r = -1;
  fprintf(stderr, "fpkg_db: full restore from '%s' %s\n", slot, r == 0 ? "OK" : "FAILED");
  return r;
}

char *fpkg_db_snapshot_list(void) {
  char buf[128];
  int n = 0, pos = 0;
  pos += snprintf(buf + pos, sizeof(buf) - pos, "[");
  if(file_size(SNAP_ROOT "/current/app.db") > 0)
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%s\"current\"", n++ ? "," : "");
  if(file_size(SNAP_ROOT "/1installback/app.db") > 0)
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%s\"1installback\"", n++ ? "," : "");
  snprintf(buf + pos, sizeof(buf) - pos, "]");
  return strdup(buf);
}

/* -------------------------------------------------- manual dated snapshots */
#define SNAP_MANUAL_ROOT SNAP_ROOT "/manual"

int fpkg_db_manual_snapshot(void) {
  mkdir(BACKUP_ROOT,       0755);
  mkdir(SNAP_ROOT,         0755);
  mkdir(SNAP_MANUAL_ROOT,  0755);

  time_t now = time(NULL);
  struct tm *tm = localtime(&now);
  char ts[32];
  strftime(ts, sizeof ts, "%Y-%m-%d_%H-%M-%S", tm);

  char dir[160];
  snprintf(dir, sizeof dir, SNAP_MANUAL_ROOT "/%s", ts);
  mkdir(dir, 0755);

  char app[200], ai[200];
  snprintf(app, sizeof app, "%s/app.db",     dir);
  snprintf(ai,  sizeof ai,  "%s/appinfo.db", dir);

  int r = 0;
  if(db_online_copy(APP_DB_PATH,     app) != 0) r = -1;
  if(db_online_copy(APPINFO_DB_PATH, ai)  != 0) r = -1;
  fprintf(stderr, "fpkg_db: manual snapshot '%s' %s\n", ts, r == 0 ? "OK" : "FAILED");
  return r;
}

/* Return JSON array of manual snapshot names, newest first (malloc'd). */
char *fpkg_db_manual_list(void) {
  DIR *d = opendir(SNAP_MANUAL_ROOT);
  if(!d) return strdup("[]");

  /* Collect valid entries (directories whose app.db exists). */
  char names[64][32];
  int  count = 0;
  struct dirent *e;
  while((e = readdir(d)) && count < 64) {
    if(e->d_name[0] == '.') continue;
    char chk[220];
    snprintf(chk, sizeof chk, SNAP_MANUAL_ROOT "/%s/app.db", e->d_name);
    if(file_size(chk) > 0) {
      strncpy(names[count], e->d_name, 31);
      names[count][31] = '\0';
      count++;
    }
  }
  closedir(d);

  /* Sort descending (newest first) — names are ISO timestamps so alpha order works. */
  for(int i = 0; i < count - 1; i++)
    for(int j = i + 1; j < count; j++)
      if(strcmp(names[i], names[j]) < 0) {
        char tmp[32];
        memcpy(tmp,       names[i], 32);
        memcpy(names[i],  names[j], 32);
        memcpy(names[j],  tmp,      32);
      }

  /* Build JSON. Each name is ~20 chars; 64 * 24 + brackets = ~1600 bytes. */
  char *buf = malloc(2048);
  if(!buf) return strdup("[]");
  int pos = 0;
  buf[pos++] = '[';
  for(int i = 0; i < count; i++) {
    pos += snprintf(buf + pos, 2048 - pos, "%s\"%s\"", i ? "," : "", names[i]);
  }
  buf[pos++] = ']';
  buf[pos]   = '\0';
  return buf;
}
