
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <microhttpd.h>

#include "sdk_changer.h"
#include "smp_updater.h"
#include "sys.h"
#include "third_party/cJSON.h"
#include "websrv.h"


#define SDK_STATUS_PATH  "/data/elf-arsenal/last-sdk-changer.json"
#define SDK_SCAN_PATH    "/data/elf-arsenal/sdk-scan.json"
#define SDK_SCAN_TMP     "/data/elf-arsenal/sdk-scan.json.tmp"


static atomic_int g_scan_in_progress = 0;


/* ── status mirror ───────────────────────────────────────────────── */

static void
write_status_str(const char *json) {
    mkdir("/data", 0755);
    mkdir("/data/elf-arsenal", 0755);
    int fd = open(SDK_STATUS_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    (void)write(fd, json, strlen(json));
    close(fd);
}



static int
json_extract_str(const char *buf, const char *key, char *out, size_t out_size) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(buf, needle);
    if (!p) return -1;
    p = strchr(p + strlen(needle), ':');
    if (!p) return -1;
    p++;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_size) {
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = 0;
    return 0;
}

static int
json_extract_num(const char *buf, const char *key, uint64_t *out) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(buf, needle);
    if (!p) return -1;
    p = strchr(p + strlen(needle), ':');
    if (!p) return -1;
    p++;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if (*p == '"') p++;
    char *end = NULL;
    uint64_t v;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        v = strtoull(p, &end, 16);
    else
        v = strtoull(p, &end, 10);
    if (end == p) return -1;
    *out = v;
    return 0;
}


/* ── minimal SFO reader (PS4 / CUSA) ──────────────────────────────── */

#define SFO_MAGIC 0x46535000   /* "\0PSF" */
struct sfo_header {
    uint32_t magic;
    uint32_t version;
    uint32_t key_table_offset;
    uint32_t data_table_offset;
    uint32_t entries_count;
};
struct sfo_entry {
    uint16_t key_offset;
    uint16_t format;
    uint32_t length;
    uint32_t max_length;
    uint32_t data_offset;
};

static int
sfo_extract(const char *path, char *title_id, size_t tid_size,
            char *title_name, size_t tn_size,
            uint32_t *sdk_ver_out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(struct sfo_header)) {
        close(fd); return -1;
    }
    void *buf = malloc(st.st_size);
    if (!buf) { close(fd); return -1; }
    ssize_t n = read(fd, buf, st.st_size);
    close(fd);
    if (n != st.st_size) { free(buf); return -1; }

    struct sfo_header *h = buf;
    if (h->magic != SFO_MAGIC) { free(buf); return -1; }

    char *kt = (char *)buf + h->key_table_offset;
    uint8_t *dt = (uint8_t *)buf + h->data_table_offset;
    struct sfo_entry *e =
        (struct sfo_entry *)((uint8_t *)buf + sizeof(struct sfo_header));

    if (title_id  && tid_size) title_id[0]  = 0;
    if (title_name && tn_size) title_name[0] = 0;
    if (sdk_ver_out) *sdk_ver_out = 0;

    for (uint32_t i = 0; i < h->entries_count; i++) {
        const char *k = kt + e[i].key_offset;
        uint8_t *d = dt + e[i].data_offset;
        if (title_id && !strcmp(k, "TITLE_ID")) {
            size_t cp = e[i].length;
            if (cp >= tid_size) cp = tid_size - 1;
            memcpy(title_id, d, cp); title_id[cp] = 0;
        } else if (title_name && !strcmp(k, "TITLE")) {
            size_t cp = e[i].length;
            if (cp >= tn_size) cp = tn_size - 1;
            memcpy(title_name, d, cp); title_name[cp] = 0;
        } else if (sdk_ver_out && !strcmp(k, "PUBTOOLINFO")) {
            const char *s = strstr((const char *)d, "sdk_ver=");
            if (s) *sdk_ver_out = (uint32_t)strtoul(s + 8, NULL, 16);
        }
    }
    free(buf);
    return 0;
}


/* ── recursive walker — finds game folders under SMP scan roots ───── */

#define MAX_SEEN_PATHS 512
#define SEEN_PATH_LEN  512
typedef struct {
    FILE *fp;
    int  first;
    int  count;
    char (*seen)[SEEN_PATH_LEN];
    int  seen_count;
} scan_ctx_t;

static int
already_seen(scan_ctx_t *sc, const char *path) {
    if (!sc->seen) return 0;
    for (int i = 0; i < sc->seen_count; i++) {
        if (!strcmp(sc->seen[i], path)) return 1;
    }
    if (sc->seen_count < MAX_SEEN_PATHS) {
        size_t L = strlen(path);
        if (L < SEEN_PATH_LEN) {
            memcpy(sc->seen[sc->seen_count], path, L + 1);
            sc->seen_count++;
        }
    }
    return 0;
}

static void
emit_game(scan_ctx_t *sc, const char *path,
          const char *title_id, const char *title_name,
          uint32_t ps5_sdk, uint32_t ps4_sdk, int is_ps4) {
    if (already_seen(sc, path)) return;
    if (!sc->first) fputs(",", sc->fp);
    sc->first = 0;
    char escaped[256] = {0};
    size_t j = 0;
    for (size_t i = 0; title_name[i] && j + 2 < sizeof(escaped); i++) {
        char c = title_name[i];
        if (c == '"' || c == '\\') escaped[j++] = '\\';
        if (c < 0x20) c = ' ';
        escaped[j++] = c;
    }
    char path_escaped[1024] = {0};
    size_t pj = 0;
    for (size_t i = 0; path[i] && pj + 2 < sizeof(path_escaped); i++) {
        char c = path[i];
        if (c == '"' || c == '\\') path_escaped[pj++] = '\\';
        path_escaped[pj++] = c;
    }
    fprintf(sc->fp,
        "{\"path\":\"%s\","
         "\"titleId\":\"%s\","
         "\"titleName\":\"%s\","
         "\"sdkPs5\":%u,"
         "\"sdkPs4\":%u,"
         "\"isPs4\":%s}",
        path_escaped, title_id, escaped, ps5_sdk, ps4_sdk,
        is_ps4 ? "true" : "false");
    sc->count++;
}

static int
maybe_process_game_dir(const char *dir, scan_ctx_t *sc) {
    char p[1024];
    snprintf(p, sizeof(p), "%s/sce_sys/param.json", dir);
    if (access(p, R_OK) == 0) {
        int fd = open(p, O_RDONLY);
        if (fd < 0) return 0;
        char buf[16384] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) return 0;
        char title_id[32] = {0}, title_name[128] = {0};
        uint64_t sdk_v = 0;
        json_extract_str(buf, "titleId", title_id, sizeof(title_id));
        json_extract_str(buf, "titleName", title_name, sizeof(title_name));
        if (!title_name[0])
            json_extract_str(buf, "title", title_name, sizeof(title_name));
        json_extract_num(buf, "sdkVersion", &sdk_v);
        /* param.json stores it as a 64-bit hex string ("0x0300000000000000");
           PT_SCE_PROCPARAM holds the same value in its upper 32 bits. */
        uint32_t sdk32 = (uint32_t)(sdk_v >> 32);
        emit_game(sc, dir, title_id, title_name, sdk32, 0, 0);
        return 1;
    }
    snprintf(p, sizeof(p), "%s/sce_sys/param.sfo", dir);
    if (access(p, R_OK) == 0) {
        char title_id[32] = {0}, title_name[128] = {0};
        uint32_t sdk_v = 0;
        if (sfo_extract(p, title_id, sizeof(title_id),
                        title_name, sizeof(title_name), &sdk_v) == 0) {
            emit_game(sc, dir, title_id, title_name, 0, sdk_v, 1);
            return 1;
        }
    }
    return 0;
}

static void
scan_recursive(const char *root, scan_ctx_t *sc, int depth) {
    if (depth > 6) return;
    if (maybe_process_game_dir(root, sc)) return;
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (e->d_name[0] == '.') continue;
        char child[1024];
        if (snprintf(child, sizeof(child), "%s/%s", root, e->d_name)
            >= (int)sizeof(child)) continue;
        struct stat st;
        if (lstat(child, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        scan_recursive(child, sc, depth + 1);
    }
    closedir(d);
}


/* ── path-dedupe across multiple SMP scan roots ───────────────────── */

#define MAX_ROOTS 128
typedef struct {
    char roots[MAX_ROOTS][256];
    int  count;
} roots_collector_t;

static void
collect_root(const char *path, void *arg) {
    roots_collector_t *rc = arg;
    if (rc->count >= MAX_ROOTS) return;
    for (int i = 0; i < rc->count; i++) {
        if (!strcmp(rc->roots[i], path)) return;
    }
    size_t L = strlen(path);
    if (L == 0 || L >= sizeof(rc->roots[0])) return;
    memcpy(rc->roots[rc->count], path, L + 1);
    rc->count++;
}


/* ── scan thread ─────────────────────────────────────────────────── */

static void *
scan_thread_fn(void *arg) {
    (void)arg;

    write_status_str("{\"ok\":true,\"pending\":true,\"kind\":\"scan\"}");

    mkdir("/data", 0755);
    mkdir("/data/elf-arsenal", 0755);

    roots_collector_t rc;
    rc.count = 0;
    smp_foreach_scan_path(collect_root, &rc);

    const char *always[] = {
        "/data/homebrew", "/data/etaHEN/games",
        "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
        "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7",
        "/mnt/ext0", "/mnt/ext1",
        NULL,
    };
    for (int i = 0; always[i]; i++) collect_root(always[i], &rc);

    FILE *fp = fopen(SDK_SCAN_TMP, "w");
    if (!fp) {
        write_status_str("{\"ok\":false,\"finished\":true,\"kind\":\"scan\","
                         "\"error\":\"cannot open scan output\"}");
        atomic_store(&g_scan_in_progress, 0);
        return NULL;
    }
    fputs("{\"games\":[", fp);

    scan_ctx_t sc = { .fp = fp, .first = 1, .count = 0 };
    sc.seen = calloc(MAX_SEEN_PATHS, SEEN_PATH_LEN);

    for (int i = 0; i < rc.count; i++) {
        struct stat st;
        if (stat(rc.roots[i], &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        fprintf(stderr, "sdk-changer: scanning %s ...\n", rc.roots[i]);
        scan_recursive(rc.roots[i], &sc, 0);
    }

    fprintf(fp, "],\"count\":%d,\"finishedAt\":%lld}",
            sc.count, (long long)time(NULL));
    fclose(fp);
    rename(SDK_SCAN_TMP, SDK_SCAN_PATH);
    free(sc.seen);

    char done[256];
    snprintf(done, sizeof(done),
        "{\"ok\":true,\"finished\":true,\"kind\":\"scan\","
         "\"count\":%d,\"finishedAt\":%lld}",
        sc.count, (long long)time(NULL));
    write_status_str(done);
    fprintf(stderr, "sdk-changer: scan complete, %d games → %s\n",
            sc.count, SDK_SCAN_PATH);

    atomic_store(&g_scan_in_progress, 0);
    return NULL;
}

static int
kick_scan(void) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_scan_in_progress, &expected, 1)) {
        return 0;
    }
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, scan_thread_fn, NULL);
    pthread_attr_destroy(&a);
    return 1;
}


/* ── HTTP plumbing ───────────────────────────────────────────────── */

static enum MHD_Result
serve_json_owned(struct MHD_Connection *conn, unsigned status, cJSON *r) {
    char *txt = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (!txt) return MHD_NO;
    size_t len = strlen(txt);
    struct MHD_Response *resp =
        MHD_create_response_from_buffer(len, txt, MHD_RESPMEM_MUST_FREE);
    if (!resp) { free(txt); return MHD_NO; }
    MHD_add_response_header(resp, "Content-Type",  "application/json");
    MHD_add_response_header(resp, "Cache-Control", "no-cache");
    enum MHD_Result rc = websrv_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    return rc;
}


static enum MHD_Result
serve_file(struct MHD_Connection *conn, const char *path,
           const char *fallback_json) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        size_t len = strlen(fallback_json);
        struct MHD_Response *resp =
            MHD_create_response_from_buffer(len, (void *)fallback_json,
                                            MHD_RESPMEM_MUST_COPY);
        if (!resp) return MHD_NO;
        MHD_add_response_header(resp, "Content-Type",  "application/json");
        MHD_add_response_header(resp, "Cache-Control", "no-cache");
        enum MHD_Result rc = websrv_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return rc;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return serve_file(conn, "/nonexistent", fallback_json);
    }
    char *buf = malloc(st.st_size);
    if (!buf) { close(fd); return MHD_NO; }
    ssize_t n = read(fd, buf, st.st_size);
    close(fd);
    if (n != st.st_size) { free(buf); return MHD_NO; }
    struct MHD_Response *resp =
        MHD_create_response_from_buffer((size_t)n, buf, MHD_RESPMEM_MUST_FREE);
    if (!resp) { free(buf); return MHD_NO; }
    MHD_add_response_header(resp, "Content-Type",  "application/json");
    MHD_add_response_header(resp, "Cache-Control", "no-cache");
    enum MHD_Result rc = websrv_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return rc;
}


enum MHD_Result
sdk_changer_request(struct MHD_Connection *conn, const char *url) {
    if (!strcmp(url, "/api/sdk-changer/scan")) {
        int started = kick_scan();
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok",      1);
        cJSON_AddBoolToObject(r, "started", started);
        cJSON_AddStringToObject(r, "statusUrl", "/api/sdk-changer/status");
        cJSON_AddStringToObject(r, "listUrl",   "/api/sdk-changer/list");
        return serve_json_owned(conn, MHD_HTTP_OK, r);
    }

    if (!strcmp(url, "/api/sdk-changer/list")) {
        return serve_file(conn, SDK_SCAN_PATH,
            "{\"ok\":true,\"games\":[],"
             "\"note\":\"no scan run yet — POST /api/sdk-changer/scan\"}");
    }

    if (!strcmp(url, "/api/sdk-changer/status")) {
        return serve_file(conn, SDK_STATUS_PATH,
            "{\"ok\":true,\"idle\":true}");
    }

    if (!strcmp(url, "/api/sdk-changer/apply")) {
        const char *path = MHD_lookup_connection_value(conn,
            MHD_GET_ARGUMENT_KIND, "path");
        const char *ps5  = MHD_lookup_connection_value(conn,
            MHD_GET_ARGUMENT_KIND, "ps5_sdk");
        const char *ps4  = MHD_lookup_connection_value(conn,
            MHD_GET_ARGUMENT_KIND, "ps4_sdk");
        if (!path || !*path) {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddBoolToObject(r, "ok", 0);
            cJSON_AddStringToObject(r, "error",
                "missing 'path' query arg");
            return serve_json_owned(conn, MHD_HTTP_BAD_REQUEST, r);
        }
        if (!ps5) ps5 = "0";
        if (!ps4) ps4 = "0";
        const char *argv[4] = { "apply", path, ps5, ps4 };
        int rc = sys_spawn_sdk_changer(4, argv);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok",      rc == 0);
        cJSON_AddBoolToObject(r, "started", rc == 0);
        cJSON_AddStringToObject(r, "path", path);
        cJSON_AddStringToObject(r, "statusUrl", "/api/sdk-changer/status");
        if (rc != 0)
            cJSON_AddStringToObject(r, "error",
                "could not spawn sdk-changer.elf");
        return serve_json_owned(conn,
            rc == 0 ? MHD_HTTP_OK : MHD_HTTP_INTERNAL_SERVER_ERROR, r);
    }

    cJSON *err = cJSON_CreateObject();
    cJSON_AddBoolToObject(err, "ok", 0);
    cJSON_AddStringToObject(err, "error", "no such endpoint");
    return serve_json_owned(conn, MHD_HTTP_NOT_FOUND, err);
}
