#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "zipread.h"

#include <microhttpd.h>

#include "offline_pack.h"
#include "ps5/http.h"
#include "third_party/cJSON.h"
#include "websrv.h"

#define PACK_RELEASE_API \
  "https://git.etawen.dev/api/v1/repos/soniciso/elf-arsenal/releases/latest"
#define PACK_ASSET_NAME_PREFIX "elf-arsenal-offline-pack"


#define PACK_ZIP_PATH      "/data/elf-arsenal-offline-pack.zip"
#define PACK_EXTRACT_ROOT  "/data"
#define PACK_MARKER_PATH   "/data/elf-arsenal/.offline-pack-mtime"
#define PACK_MANIFEST_PATH "/data/elf-arsenal/offline-pack/manifest.json"


static atomic_int g_offline_enabled  = 0;
static atomic_int g_dl_in_progress   = 0;
static atomic_int g_dl_last_status   = 0;
static char       g_dl_last_error[256] = {0};

static int kick_download(void);


int  offline_pack_get_enabled(void) { return atomic_load(&g_offline_enabled); }
void offline_pack_set_enabled(int on) {
    atomic_store(&g_offline_enabled, on ? 1 : 0);
    extern void config_save(void);
    config_save();
}

int offline_pack_is_installed(void) {
    struct stat st;
    return stat(PACK_MANIFEST_PATH, &st) == 0 && st.st_size > 0;
}


static int
read_marker(time_t *mtime_out) {
    FILE *f = fopen(PACK_MARKER_PATH, "r");
    if (!f) return 0;
    long long v = 0;
    int rc = fscanf(f, "%lld", &v);
    fclose(f);
    if (rc != 1) return 0;
    *mtime_out = (time_t)v;
    return 1;
}

static void
write_marker(time_t mtime) {
    FILE *f = fopen(PACK_MARKER_PATH, "w");
    if (!f) return;
    fprintf(f, "%lld\n", (long long)mtime);
    fclose(f);
}


static int
mkdir_p(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len && tmp[len - 1] == '/') tmp[len - 1] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
    return 0;
}


static int
extract_zip(const char *zip_path, const char *dst_root) {
    ezip *a = ezip_open_file(zip_path);
    if (ezip_failed(a)) {
        fprintf(stderr, "offline_pack: open %s: %s\n",
                zip_path, ezip_error(a));
        ezip_close(a);
        return -1;
    }

    int errors = 0;
    ezip_entry e;
    while (ezip_next(a, &e) == 1) {
        const char *name = e.path;
        if (!name || strstr(name, "..")) continue;

        char out_path[1280];
        snprintf(out_path, sizeof(out_path), "%s/%s", dst_root, name);

        if (e.is_dir) {
            mkdir_p(out_path);
            continue;
        }

        char dir[1280];
        snprintf(dir, sizeof(dir), "%s", out_path);
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = 0; mkdir_p(dir); }

        int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { errors++; continue; }

        ezip_stream *s = ezip_stream_open(a);
        if (!s) { close(fd); errors++; continue; }
        uint8_t buf[16 * 1024];
        ssize_t n;
        while ((n = ezip_stream_read(s, buf, sizeof(buf))) > 0) {
            if (write(fd, buf, (size_t)n) != n) { errors++; break; }
        }
        ezip_stream_close(s);
        close(fd);
    }

    ezip_close(a);
    return errors == 0 ? 0 : -1;
}


static int
file_to_buf(const char *path, uint8_t **out, size_t *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return -1; }
    uint8_t *b = malloc(st.st_size);
    if (!b) { close(fd); return -1; }
    ssize_t n = read(fd, b, st.st_size);
    close(fd);
    if (n != st.st_size) { free(b); return -1; }
    *out = b;
    *out_len = (size_t)st.st_size;
    return 0;
}


static cJSON *
load_manifest(void) {
    uint8_t *buf = NULL; size_t len = 0;
    if (file_to_buf(PACK_MANIFEST_PATH, &buf, &len) != 0) return NULL;
    char *s = malloc(len + 1);
    if (!s) { free(buf); return NULL; }
    memcpy(s, buf, len);
    s[len] = 0;
    free(buf);
    cJSON *m = cJSON_Parse(s);
    free(s);
    return m;
}


int
offline_pack_get_release_list(const char *tool,
                              uint8_t **out_buf, size_t *out_len) {
    if (!tool || !out_buf || !out_len) return -1;
    if (!offline_pack_is_installed()) return -1;

    cJSON *m = load_manifest();
    if (!m) return -1;
    cJSON *tools = cJSON_GetObjectItemCaseSensitive(m, "tools");
    cJSON *entry = tools ? cJSON_GetObjectItemCaseSensitive(tools, tool) : NULL;
    cJSON *versions = entry ? cJSON_GetObjectItemCaseSensitive(entry, "versions") : NULL;
    if (!cJSON_IsArray(versions)) { cJSON_Delete(m); return -1; }

    cJSON *out = cJSON_CreateArray();
    cJSON *v;
    cJSON_ArrayForEach(v, versions) {
        const cJSON *tag = cJSON_GetObjectItemCaseSensitive(v, "tag");
        const cJSON *asset = cJSON_GetObjectItemCaseSensitive(v, "asset");
        const cJSON *pub = cJSON_GetObjectItemCaseSensitive(v, "publishedAt");
        if (!cJSON_IsString(tag) || !cJSON_IsString(asset)) continue;

        cJSON *rel = cJSON_CreateObject();
        cJSON_AddStringToObject(rel, "tag_name", tag->valuestring);
        cJSON_AddStringToObject(rel, "name", tag->valuestring);
        if (cJSON_IsString(pub))
            cJSON_AddStringToObject(rel, "published_at", pub->valuestring);
        cJSON_AddBoolToObject(rel, "prerelease", 0);
        cJSON_AddBoolToObject(rel, "draft", 0);

        cJSON *assets = cJSON_CreateArray();
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "name", asset->valuestring);
        char url[512];
        snprintf(url, sizeof(url),
                 "/api/offline/asset?tool=%s&tag=%s&asset=%s",
                 tool, tag->valuestring, asset->valuestring);
        cJSON_AddStringToObject(a, "browser_download_url", url);
        cJSON_AddItemToArray(assets, a);
        cJSON_AddItemToObject(rel, "assets", assets);

        cJSON_AddItemToArray(out, rel);
    }
    cJSON_Delete(m);

    char *s = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (!s) return -1;
    *out_len = strlen(s);
    *out_buf = (uint8_t *)s;
    return 0;
}


int
offline_pack_get_asset(const char *tool, const char *tag, const char *asset,
                       uint8_t **out_buf, size_t *out_len) {
    if (!tool || !tag || !asset) return -1;
    if (strstr(tool, "..") || strstr(tag, "..") || strstr(asset, "..")) return -1;
    if (strchr(tool, '/') || strchr(tag, '/') || strchr(asset, '/')) return -1;

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s/%s/%s",
             PACK_EXTRACT_ROOT, tool, tag, asset);
    return file_to_buf(path, out_buf, out_len);
}


static enum MHD_Result
serve_buf(struct MHD_Connection *conn, unsigned code,
          const char *ctype, uint8_t *buf, size_t len) {
    struct MHD_Response *resp =
        MHD_create_response_from_buffer(len, buf, MHD_RESPMEM_MUST_FREE);
    if (!resp) { free(buf); return MHD_NO; }
    MHD_add_response_header(resp, "Content-Type", ctype);
    MHD_add_response_header(resp, "Cache-Control", "no-cache");
    enum MHD_Result rc = websrv_queue_response(conn, code, resp);
    MHD_destroy_response(resp);
    return rc;
}


static enum MHD_Result
serve_json_str(struct MHD_Connection *conn, unsigned code, const char *s) {
    size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (!copy) return MHD_NO;
    memcpy(copy, s, len + 1);
    return serve_buf(conn, code, "application/json", (uint8_t *)copy, len);
}


enum MHD_Result
offline_pack_request(struct MHD_Connection *conn, const char *url) {
    if (!strcmp(url, "/api/offline/status")) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", 1);
        cJSON_AddBoolToObject(r, "enabled",   offline_pack_get_enabled());
        cJSON_AddBoolToObject(r, "installed", offline_pack_is_installed());

        struct stat zs;
        cJSON_AddBoolToObject(r, "zipPresent",
                              stat(PACK_ZIP_PATH, &zs) == 0);
        if (stat(PACK_ZIP_PATH, &zs) == 0) {
            cJSON_AddNumberToObject(r, "zipBytes", (double)zs.st_size);
            cJSON_AddNumberToObject(r, "zipMtime", (double)zs.st_mtime);
        }
        cJSON_AddStringToObject(r, "zipPath", PACK_ZIP_PATH);

        cJSON_AddBoolToObject(r, "downloadInProgress",
                              atomic_load(&g_dl_in_progress));
        int dl_s = atomic_load(&g_dl_last_status);
        cJSON_AddStringToObject(r, "downloadLastStatus",
            dl_s == 1 ? "ok" : dl_s == -1 ? "error" : "idle");
        if (g_dl_last_error[0])
            cJSON_AddStringToObject(r, "downloadLastError", g_dl_last_error);

        cJSON *m = load_manifest();
        if (m) {
            cJSON *tools = cJSON_GetObjectItemCaseSensitive(m, "tools");
            cJSON *summary = cJSON_CreateObject();
            if (cJSON_IsObject(tools)) {
                cJSON *t;
                cJSON_ArrayForEach(t, tools) {
                    cJSON *vs = cJSON_GetObjectItemCaseSensitive(t, "versions");
                    int n = cJSON_IsArray(vs) ? cJSON_GetArraySize(vs) : 0;
                    cJSON_AddNumberToObject(summary, t->string, n);
                }
            }
            cJSON_AddItemToObject(r, "versionCounts", summary);
            const cJSON *built = cJSON_GetObjectItemCaseSensitive(m, "builtAt");
            if (cJSON_IsString(built))
                cJSON_AddStringToObject(r, "builtAt", built->valuestring);
            cJSON_Delete(m);
        }

        char *s = cJSON_PrintUnformatted(r);
        cJSON_Delete(r);
        if (!s) return MHD_NO;
        return serve_buf(conn, MHD_HTTP_OK, "application/json",
                         (uint8_t *)s, strlen(s));
    }

    if (!strcmp(url, "/api/offline/toggle")) {
        const char *on = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "on");
        offline_pack_set_enabled(on && (*on == '1' || *on == 't' || *on == 'T'));
        return serve_json_str(conn, MHD_HTTP_OK,
            offline_pack_get_enabled()
                ? "{\"ok\":true,\"enabled\":true}"
                : "{\"ok\":true,\"enabled\":false}");
    }

    if (!strcmp(url, "/api/offline/download")) {
        int started = kick_download();
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", 1);
        cJSON_AddBoolToObject(r, "started", started);
        cJSON_AddStringToObject(r, "src", PACK_RELEASE_API);
        cJSON_AddStringToObject(r, "dest", PACK_ZIP_PATH);
        if (!started)
            cJSON_AddStringToObject(r, "note", "a download is already in flight");
        char *s = cJSON_PrintUnformatted(r);
        cJSON_Delete(r);
        if (!s) return MHD_NO;
        return serve_buf(conn, MHD_HTTP_OK, "application/json",
                         (uint8_t *)s, strlen(s));
    }

    if (!strcmp(url, "/api/offline/extract")) {
        struct stat zs;
        if (stat(PACK_ZIP_PATH, &zs) != 0) {
            return serve_json_str(conn, MHD_HTTP_NOT_FOUND,
                "{\"ok\":false,\"error\":\"no zip at "
                PACK_ZIP_PATH "\"}");
        }
        mkdir_p(PACK_EXTRACT_ROOT);
        if (extract_zip(PACK_ZIP_PATH, PACK_EXTRACT_ROOT) != 0) {
            return serve_json_str(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                "{\"ok\":false,\"error\":\"extract failed (see klog)\"}");
        }
        write_marker(zs.st_mtime);
        return serve_json_str(conn, MHD_HTTP_OK,
            "{\"ok\":true,\"extracted\":true}");
    }

    if (!strcmp(url, "/api/offline/list")) {
        const char *tool = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "tool");
        if (!tool || !*tool) {
            return serve_json_str(conn, MHD_HTTP_BAD_REQUEST,
                "{\"ok\":false,\"error\":\"missing 'tool' arg\"}");
        }
        uint8_t *buf = NULL; size_t len = 0;
        if (offline_pack_get_release_list(tool, &buf, &len) != 0) {
            return serve_json_str(conn, MHD_HTTP_NOT_FOUND,
                "{\"ok\":false,\"error\":\"tool not in pack manifest\"}");
        }
        return serve_buf(conn, MHD_HTTP_OK, "application/json", buf, len);
    }

    if (!strcmp(url, "/api/offline/asset")) {
        const char *tool  = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "tool");
        const char *tag   = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "tag");
        const char *asset = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "asset");
        if (!tool || !tag || !asset) {
            return serve_json_str(conn, MHD_HTTP_BAD_REQUEST,
                "{\"ok\":false,\"error\":\"need tool, tag, asset\"}");
        }
        uint8_t *buf = NULL; size_t len = 0;
        if (offline_pack_get_asset(tool, tag, asset, &buf, &len) != 0) {
            return serve_json_str(conn, MHD_HTTP_NOT_FOUND,
                "{\"ok\":false,\"error\":\"asset not in pack\"}");
        }
        return serve_buf(conn, MHD_HTTP_OK,
                         "application/octet-stream", buf, len);
    }

    return serve_json_str(conn, MHD_HTTP_NOT_FOUND,
        "{\"ok\":false,\"error\":\"no such offline endpoint\"}");
}


static char *
pick_pack_asset_url(const char *json, size_t json_len) {
    char *txt = malloc(json_len + 1);
    if (!txt) return NULL;
    memcpy(txt, json, json_len); txt[json_len] = 0;
    cJSON *root = cJSON_Parse(txt);
    free(txt);
    if (!root) return NULL;
    char *url = NULL;
    cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
    if (cJSON_IsArray(assets)) {
        cJSON *a;
        cJSON_ArrayForEach(a, assets) {
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(a, "name");
            const cJSON *u    = cJSON_GetObjectItemCaseSensitive(a, "browser_download_url");
            if (!cJSON_IsString(u)) continue;
            if (cJSON_IsString(name) &&
                strncmp(name->valuestring, PACK_ASSET_NAME_PREFIX,
                        strlen(PACK_ASSET_NAME_PREFIX)) == 0) {
                url = strdup(u->valuestring);
                break;
            }
        }
    }
    cJSON_Delete(root);
    return url;
}

static void *
offline_download_thread(void *arg) {
    (void)arg;

    g_dl_last_error[0] = 0;
    atomic_store(&g_dl_last_status, 0);

    fprintf(stderr, "offline_pack: fetching release manifest %s\n",
            PACK_RELEASE_API);
    size_t json_len = 0;
    uint8_t *json = http_get(PACK_RELEASE_API, &json_len);
    if (!json || json_len < 32) {
        snprintf(g_dl_last_error, sizeof(g_dl_last_error),
                 "release manifest fetch failed (DNS?)");
        atomic_store(&g_dl_last_status, -1);
        free(json);
        atomic_store(&g_dl_in_progress, 0);
        return NULL;
    }
    char *asset_url = pick_pack_asset_url((const char *)json, json_len);
    free(json);
    if (!asset_url) {
        snprintf(g_dl_last_error, sizeof(g_dl_last_error),
                 "no offline-pack asset on latest release");
        atomic_store(&g_dl_last_status, -1);
        atomic_store(&g_dl_in_progress, 0);
        return NULL;
    }

    fprintf(stderr, "offline_pack: downloading %s\n", asset_url);
    size_t zip_len = 0;
    uint8_t *zip = http_get(asset_url, &zip_len);
    free(asset_url);
    if (!zip || zip_len < 1024) {
        snprintf(g_dl_last_error, sizeof(g_dl_last_error),
                 "asset download returned no body");
        atomic_store(&g_dl_last_status, -1);
        free(zip);
        atomic_store(&g_dl_in_progress, 0);
        return NULL;
    }

    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", PACK_ZIP_PATH);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(g_dl_last_error, sizeof(g_dl_last_error),
                 "cannot create %s", tmp);
        atomic_store(&g_dl_last_status, -1);
        free(zip);
        atomic_store(&g_dl_in_progress, 0);
        return NULL;
    }
    ssize_t n = write(fd, zip, zip_len);
    close(fd);
    free(zip);
    if (n != (ssize_t)zip_len) {
        unlink(tmp);
        snprintf(g_dl_last_error, sizeof(g_dl_last_error),
                 "short write (disk full?)");
        atomic_store(&g_dl_last_status, -1);
        atomic_store(&g_dl_in_progress, 0);
        return NULL;
    }
    if (rename(tmp, PACK_ZIP_PATH) != 0) {
        unlink(tmp);
        snprintf(g_dl_last_error, sizeof(g_dl_last_error),
                 "rename failed: %s", strerror(errno));
        atomic_store(&g_dl_last_status, -1);
        atomic_store(&g_dl_in_progress, 0);
        return NULL;
    }

    fprintf(stderr, "offline_pack: download OK (%zu bytes -> %s)\n",
            zip_len, PACK_ZIP_PATH);
    atomic_store(&g_dl_last_status, 1);
    atomic_store(&g_dl_in_progress, 0);
    return NULL;
}

static int
kick_download(void) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_dl_in_progress, &expected, 1)) {
        return 0;
    }
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, offline_download_thread, NULL);
    pthread_attr_destroy(&a);
    return 1;
}

static void *
offline_extract_thread(void *arg) {
    (void)arg;

    struct stat zs;
    if (stat(PACK_ZIP_PATH, &zs) != 0) return NULL;
    if (zs.st_size <= 0) return NULL;

    time_t marker = 0;
    if (read_marker(&marker) && marker == zs.st_mtime) return NULL;

    mkdir_p(PACK_EXTRACT_ROOT);

    fprintf(stderr, "offline_pack: extracting %s -> %s (%lld bytes)\n",
            PACK_ZIP_PATH, PACK_EXTRACT_ROOT, (long long)zs.st_size);

    if (extract_zip(PACK_ZIP_PATH, PACK_EXTRACT_ROOT) == 0) {
        write_marker(zs.st_mtime);
        fprintf(stderr, "offline_pack: extract OK\n");
    } else {
        fprintf(stderr, "offline_pack: extract FAILED\n");
    }
    return NULL;
}

void
offline_pack_init(void) {
    /* Only do cheap synchronous work here so boot can never hang or
       crash on a malformed / partially-written zip. The actual extract
       is detached so any libarchive failure stays local to the thread. */
    mkdir("/data", 0755);
    mkdir("/data/elf-arsenal", 0755);

    struct stat zs;
    if (stat(PACK_ZIP_PATH, &zs) != 0) return;
    if (zs.st_size <= 0) return;

    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, offline_extract_thread, NULL);
    pthread_attr_destroy(&a);
}
