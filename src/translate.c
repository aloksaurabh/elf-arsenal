#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <microhttpd.h>

#include "ps5/http.h"
#include "third_party/cJSON.h"
#include "translate.h"
#include "websrv.h"


#define I18N_DIR  "/data/elf-arsenal/i18n"
#define DEBUG_LOG "/data/elf-arsenal/translate-debug.log"

/* File-based debug trace. elf-arsenal's stderr/printf only reaches the
   elfldr deploy socket (which closes after boot), and klogsrv carries
   only kernel-level messages — so when websrv crashes mid-translation
   we have NO way to see what was happening unless we wrote it to disk
   first. This log is append-only so the file survives a crash + reboot,
   and is rotated by hand (just delete it). */
static void
dbg_log(const char *fmt, ...) {
    static pthread_mutex_t dbg_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&dbg_lock);
    mkdir("/data", 0755);
    mkdir("/data/elf-arsenal", 0755);
    FILE *fp = fopen(DEBUG_LOG, "a");
    if (fp) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        fprintf(fp, "[%ld.%03ld] ", (long)ts.tv_sec, (long)(ts.tv_nsec / 1000000L));
        va_list ap;
        va_start(ap, fmt);
        vfprintf(fp, fmt, ap);
        va_end(ap);
        fputc('\n', fp);
        fclose(fp);
    }
    pthread_mutex_unlock(&dbg_lock);
}


static pthread_mutex_t g_cache_lock = PTHREAD_MUTEX_INITIALIZER;

/* http_get() ultimately drives sceNetInit/sceSslInit/sceHttpInit, and
   the Sony stack isn't safe to call from N worker threads simultaneously
   — concurrent calls have been observed to crash the entire websrv
   process. Serialize every outbound HTTPS round-trip behind this mutex.
   Translations are slow anyway; correctness wins over throughput. */
static pthread_mutex_t g_http_lock  = PTHREAD_MUTEX_INITIALIZER;


static int
ensure_i18n_dir(void) {
    mkdir("/data", 0755);
    mkdir("/data/elf-arsenal", 0755);
    return mkdir(I18N_DIR, 0755);
}


static int
valid_lang_code(const char *s) {
    if (!s || !*s) return 0;
    size_t n = strlen(s);
    if (n < 2 || n > 8) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) return 0;
    }
    return 1;
}


/* URL-encode src into dst (capped at cap-1 bytes, NUL-terminated).
   Returns number of bytes written excluding terminator, or -1 on overflow. */
static int
url_encode(const char *src, char *dst, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        unsigned char c = *p;
        int safe = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
                || (c >= 'a' && c <= 'z') || c == '-' || c == '_'
                || c == '.' || c == '~';
        size_t need = safe ? 1 : 3;
        if (o + need + 1 > cap) return -1;
        if (safe) {
            dst[o++] = (char)c;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 0x0f];
        }
    }
    dst[o] = '\0';
    return (int)o;
}


/* Pull the first translated chunk out of Google's response:
 *   [[["translated","original",null,null,1], ...], null, "en"]
 * cJSON parses this fine. We concatenate every chunk[0] in the first
 * array — long inputs come back as multiple chunks. */
static char *
parse_google_response(const char *body, size_t len) {
    if (!body || len == 0) return NULL;
    cJSON *root = cJSON_ParseWithLength(body, len);
    if (!root || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON *outer = cJSON_GetArrayItem(root, 0);
    if (!cJSON_IsArray(outer)) {
        cJSON_Delete(root);
        return NULL;
    }
    size_t cap = 1024, off = 0;
    char *out = malloc(cap);
    if (!out) { cJSON_Delete(root); return NULL; }
    out[0] = '\0';
    cJSON *chunk = NULL;
    cJSON_ArrayForEach(chunk, outer) {
        if (!cJSON_IsArray(chunk)) continue;
        cJSON *piece = cJSON_GetArrayItem(chunk, 0);
        if (!cJSON_IsString(piece) || !piece->valuestring) continue;
        size_t pl = strlen(piece->valuestring);
        if (off + pl + 1 > cap) {
            while (off + pl + 1 > cap) cap *= 2;
            char *n = realloc(out, cap);
            if (!n) { free(out); cJSON_Delete(root); return NULL; }
            out = n;
        }
        memcpy(out + off, piece->valuestring, pl);
        off += pl;
        out[off] = '\0';
    }
    cJSON_Delete(root);
    if (off == 0) { free(out); return NULL; }
    return out;
}


static char *
google_translate(const char *to, const char *q) {
    char enc_q[8192];
    if (url_encode(q, enc_q, sizeof enc_q) < 0) return NULL;

    char enc_to[16];
    if (url_encode(to, enc_to, sizeof enc_to) < 0) return NULL;

    char url[10240];
    int n = snprintf(url, sizeof url,
        "https://translate.googleapis.com/translate_a/single"
        "?client=gtx&sl=auto&dt=t&tl=%s&q=%s",
        enc_to, enc_q);
    if (n < 0 || n >= (int)sizeof url) return NULL;

    /* Serialize every call into the Sony HTTP/SSL stack — see
       g_http_lock comment for why. */
    pthread_mutex_lock(&g_http_lock);
    size_t len = 0;
    uint8_t *body = http_get(url, &len);
    pthread_mutex_unlock(&g_http_lock);
    if (!body || len == 0) { free(body); return NULL; }
    char *out = parse_google_response((const char *)body, len);
    free(body);
    return out;
}


/* Batch translate via Google's translate_a/t endpoint.
 *   ?client=gtx&sl=auto&tl=es&q=hello&q=world  →  [["Hola","en"],["mundo","en"]]
 *
 * Returns a malloc'd cJSON ARRAY of strings (one per input) on success,
 * NULL on any failure. Caller must cJSON_Delete the result.
 * `srcs` is an array of N strings to translate, all in one HTTP call.
 */
static cJSON *
google_translate_batch(const char *to, char **srcs, int n) {
    dbg_log("google_translate_batch ENTER to=%s n=%d", to ? to : "(null)", n);
    if (n <= 0) return NULL;
    char enc_to[16];
    if (url_encode(to, enc_to, sizeof enc_to) < 0) { dbg_log("  url_encode(to) failed"); return NULL; }

    /* Build URL with multiple q= params. Cap at 32 KiB which is well
       above the ~8 KiB practical limit of Google's gtx endpoint —
       caller is expected to keep n × strlen(q) under ~6000 chars. */
    size_t url_cap = 32 * 1024;
    char *url = malloc(url_cap);
    if (!url) return NULL;
    int off = snprintf(url, url_cap,
        "https://translate.googleapis.com/translate_a/t"
        "?client=gtx&sl=auto&tl=%s", enc_to);
    if (off < 0 || (size_t)off >= url_cap) { free(url); return NULL; }

    char enc[8192];
    for (int i = 0; i < n; i++) {
        if (!srcs[i] || url_encode(srcs[i], enc, sizeof enc) < 0) {
            free(url); return NULL;
        }
        int added = snprintf(url + off, url_cap - off, "&q=%s", enc);
        if (added < 0 || (size_t)(off + added) >= url_cap) {
            free(url); return NULL;
        }
        off += added;
    }

    dbg_log("  url built: %d bytes, locking http_lock", off);
    pthread_mutex_lock(&g_http_lock);
    dbg_log("  http_lock acquired, calling http_get");
    size_t len = 0;
    uint8_t *body = http_get(url, &len);
    dbg_log("  http_get returned, len=%zu body=%p", len, body);
    pthread_mutex_unlock(&g_http_lock);
    free(url);
    if (!body || len == 0) { dbg_log("  empty body — FAIL"); free(body); return NULL; }

    /* Response shape: [["translated","src_lang"], ["translated","src_lang"], …]
       OR — when n == 1 — Google sometimes returns a single inner array
       instead of an array-of-arrays. Handle both. */
    dbg_log("  parsing JSON (%zu bytes)", len);
    cJSON *root = cJSON_ParseWithLength((const char *)body, len);
    free(body);
    if (!root || !cJSON_IsArray(root)) { dbg_log("  JSON parse failed"); cJSON_Delete(root); return NULL; }

    cJSON *out = cJSON_CreateArray();
    if (!out) { cJSON_Delete(root); return NULL; }

    int got = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        const char *s = NULL;
        if (cJSON_IsArray(item)) {
            cJSON *first = cJSON_GetArrayItem(item, 0);
            if (cJSON_IsString(first) && first->valuestring) s = first->valuestring;
        } else if (cJSON_IsString(item) && item->valuestring) {
            s = item->valuestring;
        }
        cJSON_AddItemToArray(out, cJSON_CreateString(s ? s : ""));
        got++;
        if (got >= n) break;
    }
    /* If Google gave us fewer entries than we asked for, pad with the
       originals so the client still gets N back (and the UI doesn't break). */
    while (got < n) {
        cJSON_AddItemToArray(out,
            cJSON_CreateString(srcs[got] ? srcs[got] : ""));
        got++;
    }

    dbg_log("  batch built, returning %d entries (asked for %d)", got, n);
    cJSON_Delete(root);
    return out;
}


/* ── on-disk cache ─────────────────────────────────────────────────
 * Layout: /data/elf-arsenal/i18n/<lang>.json
 *   { "english source string": "translated string", ... }
 * The browser ALSO keeps a copy in localStorage so warm boots don't
 * even hit websrv. The disk copy survives cache clears + factory reset
 * (since /data persists). */

static char *
cache_path_for(const char *lang) {
    char *p = NULL;
    if (asprintf(&p, "%s/%s.json", I18N_DIR, lang) < 0) return NULL;
    return p;
}


static cJSON *
load_cache(const char *lang) {
    char *path = cache_path_for(lang);
    if (!path) return cJSON_CreateObject();
    int fd = open(path, O_RDONLY);
    free(path);
    if (fd < 0) return cJSON_CreateObject();
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return cJSON_CreateObject(); }
    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) { close(fd); return cJSON_CreateObject(); }
    ssize_t r = read(fd, buf, (size_t)st.st_size);
    close(fd);
    if (r != (ssize_t)st.st_size) { free(buf); return cJSON_CreateObject(); }
    buf[st.st_size] = '\0';
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j || !cJSON_IsObject(j)) {
        cJSON_Delete(j);
        return cJSON_CreateObject();
    }
    return j;
}


static void
save_cache(const char *lang, cJSON *map) {
    ensure_i18n_dir();
    char *path = cache_path_for(lang);
    if (!path) return;
    char *tmp = NULL;
    if (asprintf(&tmp, "%s.tmp", path) < 0) { free(path); return; }
    char *txt = cJSON_PrintUnformatted(map);
    if (txt) {
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            (void)write(fd, txt, strlen(txt));
            close(fd);
            (void)rename(tmp, path);
        }
        free(txt);
    }
    free(tmp);
    free(path);
}


static char *
lookup_or_translate(const char *lang, const char *q) {
    pthread_mutex_lock(&g_cache_lock);
    cJSON *map = load_cache(lang);
    cJSON *hit = cJSON_GetObjectItemCaseSensitive(map, q);
    if (cJSON_IsString(hit) && hit->valuestring) {
        char *dup = strdup(hit->valuestring);
        cJSON_Delete(map);
        pthread_mutex_unlock(&g_cache_lock);
        return dup;
    }
    pthread_mutex_unlock(&g_cache_lock);

    /* network call without holding the cache lock */
    char *t = google_translate(lang, q);
    if (!t) {
        cJSON_Delete(map);
        return NULL;
    }

    pthread_mutex_lock(&g_cache_lock);
    /* reload in case another thread also wrote */
    cJSON_Delete(map);
    map = load_cache(lang);
    cJSON_DeleteItemFromObjectCaseSensitive(map, q);
    cJSON_AddStringToObject(map, q, t);
    save_cache(lang, map);
    cJSON_Delete(map);
    pthread_mutex_unlock(&g_cache_lock);

    return t;
}


/* ── HTTP plumbing ─────────────────────────────────────────────── */

static enum MHD_Result
serve_json(struct MHD_Connection *conn, unsigned status, cJSON *r) {
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
serve_err(struct MHD_Connection *conn, unsigned status, const char *msg) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 0);
    cJSON_AddStringToObject(o, "error", msg ? msg : "error");
    return serve_json(conn, status, o);
}


/* Batch path: client sends `q=<str1>\x1F<str2>\x1F<str3>` (US, 0x1F, the
   ASCII Unit Separator — guaranteed to never appear in regular text),
   we split, translate the not-yet-cached subset in one Google call,
   write each translation to the on-disk cache, and return JSON
   { ok:1, t:["…","…","…"] } in input order (cache hits + new translations
   merged). */
static enum MHD_Result
handle_batch(struct MHD_Connection *conn, const char *to, const char *q) {
    size_t q_bytes = strlen(q);
    dbg_log("handle_batch ENTER to=%s q_bytes=%zu", to ? to : "(null)", q_bytes);
    char *buf = strdup(q);
    if (!buf) return serve_err(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "oom");

    /* Split on US (0x1F). */
    int cap = 16, n = 0;
    char **parts = malloc(cap * sizeof(char *));
    if (!parts) { free(buf); return serve_err(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "oom"); }
    char *p = buf;
    parts[n++] = p;
    while (*p) {
        if (*p == 0x1F) {
            *p = '\0';
            if (n >= cap) {
                cap *= 2;
                char **np = realloc(parts, cap * sizeof(char *));
                if (!np) { free(parts); free(buf); return serve_err(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "oom"); }
                parts = np;
            }
            parts[n++] = p + 1;
        }
        p++;
    }
    dbg_log("  split into %d parts", n);
    if (n > 100) { free(parts); free(buf);
        return serve_err(conn, MHD_HTTP_BAD_REQUEST, "too many strings in one batch (max 100)");
    }

    /* For each input: check disk cache, collect misses. */
    pthread_mutex_lock(&g_cache_lock);
    cJSON *map = load_cache(to);
    pthread_mutex_unlock(&g_cache_lock);

    char **misses     = malloc(n * sizeof(char *));
    int   *miss_idx   = malloc(n * sizeof(int));
    char **results    = calloc(n, sizeof(char *));
    int    miss_count = 0;
    if (!misses || !miss_idx || !results) {
        free(misses); free(miss_idx); free(results);
        cJSON_Delete(map); free(parts); free(buf);
        return serve_err(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "oom");
    }
    for (int i = 0; i < n; i++) {
        cJSON *hit = cJSON_GetObjectItemCaseSensitive(map, parts[i]);
        if (cJSON_IsString(hit) && hit->valuestring) {
            results[i] = strdup(hit->valuestring);
        } else {
            misses[miss_count]   = parts[i];
            miss_idx[miss_count] = i;
            miss_count++;
        }
    }

    dbg_log("  cache lookups done: %d/%d misses", miss_count, n);
    /* Fire ONE Google call for all misses. */
    if (miss_count > 0) {
        dbg_log("  calling google_translate_batch with %d misses", miss_count);
        cJSON *batch = google_translate_batch(to, misses, miss_count);
        dbg_log("  google_translate_batch returned %p", batch);
        if (!batch) {
            for (int i = 0; i < n; i++) free(results[i]);
            free(misses); free(miss_idx); free(results);
            cJSON_Delete(map); free(parts); free(buf);
            return serve_err(conn, MHD_HTTP_BAD_GATEWAY,
                             "translate.googleapis.com unreachable (DNS? offline?)");
        }
        /* Apply translations + write back to disk cache. */
        pthread_mutex_lock(&g_cache_lock);
        cJSON_Delete(map);
        map = load_cache(to);
        for (int i = 0; i < miss_count; i++) {
            cJSON *it = cJSON_GetArrayItem(batch, i);
            const char *t = (cJSON_IsString(it) && it->valuestring) ? it->valuestring : misses[i];
            results[miss_idx[i]] = strdup(t);
            cJSON_DeleteItemFromObjectCaseSensitive(map, misses[i]);
            cJSON_AddStringToObject(map, misses[i], t);
        }
        dbg_log("  saving cache (%d new entries)", miss_count);
        save_cache(to, map);
        pthread_mutex_unlock(&g_cache_lock);
        cJSON_Delete(batch);
        dbg_log("  cache saved + batch freed");
    }

    cJSON_Delete(map);
    free(misses); free(miss_idx);

    /* Build response. */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 1);
    cJSON_AddNumberToObject(o, "n",   n);
    cJSON_AddNumberToObject(o, "new", miss_count);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(results[i] ? results[i] : ""));
        free(results[i]);
    }
    cJSON_AddItemToObject(o, "t", arr);
    free(results);
    free(parts); free(buf);
    dbg_log("handle_batch EXIT OK");
    return serve_json(conn, MHD_HTTP_OK, o);
}


enum MHD_Result
translate_request(struct MHD_Connection *conn, const char *url) {
    /* Endpoints:
     *   /api/translate?to=<lang>&q=<text>       → translate one string
     *   /api/translate/batch?to=<lang>&q=…      → translate N strings in
     *                                              one HTTP round-trip
     *                                              (strings separated by
     *                                              ASCII US, 0x1F)
     *   /api/translate/cache?to=<lang>          → dump on-disk cache
     *   /api/translate/clear?to=<lang>          → wipe on-disk cache
     */

    if (strncmp(url, "/api/translate", 14) != 0) {
        return serve_err(conn, MHD_HTTP_NOT_FOUND, "unknown endpoint");
    }

    /* The debuglog endpoint doesn't take a language; serve it before
       the `to` validation below. */
    if (!strcmp(url, "/api/translate/debuglog")) {
        int fd = open(DEBUG_LOG, O_RDONLY);
        if (fd < 0) {
            return serve_err(conn, MHD_HTTP_NOT_FOUND, "no debug log yet");
        }
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size <= 0) {
            close(fd);
            return serve_err(conn, MHD_HTTP_NOT_FOUND, "empty log");
        }
        char *body = malloc(st.st_size + 1);
        if (!body) { close(fd); return serve_err(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "oom"); }
        ssize_t r = read(fd, body, st.st_size);
        close(fd);
        if (r != (ssize_t)st.st_size) { free(body); return serve_err(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "read short"); }
        body[r] = '\0';
        struct MHD_Response *resp = MHD_create_response_from_buffer(r, body, MHD_RESPMEM_MUST_FREE);
        if (!resp) { free(body); return MHD_NO; }
        MHD_add_response_header(resp, "Content-Type", "text/plain; charset=utf-8");
        MHD_add_response_header(resp, "Cache-Control", "no-store");
        enum MHD_Result rc = websrv_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return rc;
    }

    const char *to = MHD_lookup_connection_value(
        conn, MHD_GET_ARGUMENT_KIND, "to");
    if (!valid_lang_code(to)) {
        return serve_err(conn, MHD_HTTP_BAD_REQUEST, "bad 'to' param");
    }

    if (!strcmp(url, "/api/translate/clear")) {
        pthread_mutex_lock(&g_cache_lock);
        char *path = cache_path_for(to);
        int rc = path ? unlink(path) : -1;
        free(path);
        pthread_mutex_unlock(&g_cache_lock);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", rc == 0 || errno == ENOENT);
        return serve_json(conn, MHD_HTTP_OK, o);
    }

    if (!strcmp(url, "/api/translate/cache")) {
        pthread_mutex_lock(&g_cache_lock);
        cJSON *map = load_cache(to);
        pthread_mutex_unlock(&g_cache_lock);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", 1);
        cJSON_AddStringToObject(o, "lang", to);
        cJSON_AddItemToObject(o, "map", map);
        return serve_json(conn, MHD_HTTP_OK, o);
    }

    if (!strcmp(url, "/api/translate/batch")) {
        const char *q = MHD_lookup_connection_value(
            conn, MHD_GET_ARGUMENT_KIND, "q");
        if (!q || !*q) {
            return serve_err(conn, MHD_HTTP_BAD_REQUEST, "missing 'q'");
        }
        if (strlen(q) > 16 * 1024) {
            return serve_err(conn, MHD_HTTP_BAD_REQUEST, "batch too large (>16KB)");
        }
        return handle_batch(conn, to, q);
    }

    const char *q = MHD_lookup_connection_value(
        conn, MHD_GET_ARGUMENT_KIND, "q");
    if (!q || !*q) {
        return serve_err(conn, MHD_HTTP_BAD_REQUEST, "missing 'q'");
    }
    if (strlen(q) > 4096) {
        return serve_err(conn, MHD_HTTP_BAD_REQUEST, "q too long");
    }

    char *t = lookup_or_translate(to, q);
    if (!t) {
        return serve_err(conn, MHD_HTTP_BAD_GATEWAY,
                         "translate.googleapis.com unreachable "
                         "(DNS? offline?)");
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 1);
    cJSON_AddStringToObject(o, "t", t);
    free(t);
    return serve_json(conn, MHD_HTTP_OK, o);
}
