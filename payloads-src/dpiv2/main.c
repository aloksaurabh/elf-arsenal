/* DPI v2 payload — Direct Package Installer HTTP bridge
 * Listens on TCP 12800 (INADDR_ANY), accepts install requests over HTTP,
 * forwards them to Arsenal's loopback install API (127.0.0.1:6969).
 *
 * Compatible with tools that target etaHEN DPI v2 on port 12800.
 *
 * Endpoints:
 *   GET  /              — simple HTML WebUI for manual URL entry
 *   POST /              — form post: url=<pkg-url> → install + JSON response
 *   GET  /install?url=  — direct install by query arg
 *   POST /api/install   — JSON body {"url":"..."} → {"res":"0"} (etaHEN compat)
 *
 * Install is always forwarded to Arsenal's in-process install path which
 * already holds system credentials, so no credential escalation needed here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

#define DPIV2_PORT     12800
#define ARSENAL_PORT   6969
#define REQ_MAX        4096
#define URL_MAX        2048

/* ── HTML WebUI ──────────────────────────────────────────────────────── */
static const char UI_HTML[] =
"<!DOCTYPE html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Elf Arsenal DPI v2</title>"
"<style>body{background:#1a1a2e;color:#eee;font-family:sans-serif;display:flex;"
"flex-direction:column;align-items:center;justify-content:center;min-height:100vh;margin:0}"
"h2{color:#4fc3f7;margin-bottom:24px}form{display:flex;flex-direction:column;gap:12px;width:90%;max-width:520px}"
"input{padding:12px;border:1px solid #4fc3f7;border-radius:6px;background:#0d0d1a;color:#eee;font-size:15px}"
"button{padding:12px;background:#4fc3f7;border:none;border-radius:6px;color:#1a1a2e;"
"font-size:16px;font-weight:bold;cursor:pointer}button:hover{background:#29b6f6}"
"#status{margin-top:14px;font-size:14px;min-height:20px;text-align:center}"
"</style></head><body>"
"<h2>Elf Arsenal — DPI v2</h2>"
"<form id=f>"
"<input id=url name=url type=url placeholder='https://example.com/game.pkg' required>"
"<button type=submit>Install PKG</button>"
"</form>"
"<div id=status></div>"
"<script>"
"document.getElementById('f').onsubmit=async function(e){"
"e.preventDefault();"
"var url=document.getElementById('url').value;"
"document.getElementById('status').textContent='Installing…';"
"try{"
"var r=await fetch('/api/install',{method:'POST',"
"headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({url:url})});"
"var j=await r.json();"
"document.getElementById('status').textContent="
"j.res==='0'?'✓ Install queued successfully':'✗ Error: '+JSON.stringify(j);"
"}catch(ex){document.getElementById('status').textContent='✗ '+ex;}"
"}"
"</script></body></html>";

/* ── URL-encode minimal pass-through (just spaces & special chars) ───── */
static void
urlencode(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for(const char *p = in; *p && o + 4 < outsz; p++) {
        unsigned char c = (unsigned char)*p;
        if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == '.' || c == '~' || c == ':' || c == '/' ||
           c == '?' || c == '=' || c == '&' || c == '%' ||
           c == '+' || c == '#' || c == '@' || c == '!') {
            out[o++] = (char)c;
        } else {
            o += (size_t)snprintf(out + o, outsz - o, "%%%02X", c);
        }
    }
    out[o] = '\0';
}

/* Extract value of a JSON string field "key":"value" */
static int
json_str(const char *json, const char *key, char *out, size_t outsz)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if(!p) return 0;
    p += strlen(needle);
    while(*p == ' ' || *p == ':' || *p == '\t') p++;
    if(*p != '"') return 0;
    p++;
    size_t i = 0;
    while(*p && *p != '"' && i + 1 < outsz) {
        if(*p == '\\') { p++; if(!*p) break; }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (int)i;
}

/* Extract URL from a form-encoded body (url=...) */
static int
form_url(const char *body, char *out, size_t outsz)
{
    const char *p = strstr(body, "url=");
    if(!p) return 0;
    p += 4;
    size_t i = 0;
    while(*p && *p != '&' && i + 1 < outsz) {
        if(*p == '+') { out[i++] = ' '; p++; }
        else if(*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            out[i++] = (char)strtol(hex, NULL, 16);
            p += 3;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return (int)i;
}

/* ── Forward install URL to Arsenal loopback, return 0 on success ────── */
static int
forward_install(const char *url)
{
    char encoded[URL_MAX];
    urlencode(url, encoded, sizeof(encoded));

    char req[URL_MAX + 256];
    int req_len = snprintf(req, sizeof(req),
        "GET /api/homebrew/install-pkg-url?url=%s HTTP/1.0\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n"
        "\r\n",
        encoded);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if(s < 0) return -1;

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(ARSENAL_PORT);

    struct timeval tv = {5, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if(connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(s);
        return -1;
    }
    if(send(s, req, (size_t)req_len, 0) < 0) {
        close(s);
        return -1;
    }

    char resp[512] = {0};
    recv(s, resp, sizeof(resp) - 1, 0);
    close(s);

    /* HTTP 200 + "ok":true means Arsenal accepted it */
    if(strstr(resp, "200") && strstr(resp, "\"ok\":true"))
        return 0;
    return -1;
}

/* ── Per-connection handler (runs in its own thread) ─────────────────── */
typedef struct { int fd; } conn_args_t;

static void *
handle_conn(void *arg)
{
    conn_args_t *ca = (conn_args_t *)arg;
    int cl = ca->fd;
    free(ca);

    char req[REQ_MAX] = {0};
    ssize_t n = recv(cl, req, sizeof(req) - 1, 0);
    if(n <= 0) { close(cl); return NULL; }
    req[n] = '\0';

    /* Parse first line: METHOD path */
    char method[8] = {0}, path[URL_MAX] = {0};
    sscanf(req, "%7s %2047s", method, path);

    /* Find request body (after \r\n\r\n) */
    const char *body = strstr(req, "\r\n\r\n");
    if(body) body += 4;
    else body = "";

    /* ── GET / ── serve WebUI */
    if(!strcmp(method, "GET") && (!strcmp(path, "/") || !strcmp(path, ""))) {
        size_t hlen = (size_t)strlen(UI_HTML);
        char hdr[256];
        int hdr_len = snprintf(hdr, sizeof(hdr),
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n", hlen);
        send(cl, hdr, (size_t)hdr_len, 0);
        send(cl, UI_HTML, hlen, 0);
        close(cl);
        return NULL;
    }

    /* ── Parse target URL from request ── */
    char url[URL_MAX] = {0};
    int got_url = 0;

    if(!strcmp(method, "GET")) {
        /* GET /install?url=... */
        const char *q = strchr(path, '?');
        if(q) got_url = form_url(q + 1, url, sizeof(url));
    } else if(!strcmp(method, "POST")) {
        /* POST /api/install — JSON body {"url":"..."} */
        if(strstr(path, "/api/install") || !strcmp(path, "/")) {
            /* Try JSON first */
            got_url = json_str(body, "url", url, sizeof(url));
            /* Fall back to form-encoded */
            if(!got_url) got_url = form_url(body, url, sizeof(url));
        }
    }

    const char *ok_resp   = "HTTP/1.0 200 OK\r\n"
                            "Content-Type: application/json\r\n"
                            "Connection: close\r\n"
                            "\r\n"
                            "{\"res\":\"0\"}";
    const char *err_resp  = "HTTP/1.0 500 Error\r\n"
                            "Content-Type: application/json\r\n"
                            "Connection: close\r\n"
                            "\r\n"
                            "{\"res\":\"-1\",\"error\":\"install failed\"}";
    const char *bad_resp  = "HTTP/1.0 400 Bad Request\r\n"
                            "Content-Type: application/json\r\n"
                            "Connection: close\r\n"
                            "\r\n"
                            "{\"res\":\"-1\",\"error\":\"missing url\"}";
    const char *not_found = "HTTP/1.0 404 Not Found\r\n"
                            "Content-Type: application/json\r\n"
                            "Connection: close\r\n"
                            "\r\n"
                            "{\"res\":\"-1\",\"error\":\"not found\"}";

    if(!strstr(path, "/install") && strcmp(path, "/") && !strstr(path, "/api/install")) {
        send(cl, not_found, strlen(not_found), 0);
        close(cl);
        return NULL;
    }

    if(!got_url || !*url) {
        send(cl, bad_resp, strlen(bad_resp), 0);
        close(cl);
        return NULL;
    }

    fprintf(stderr, "[dpiv2] install request: %s\n", url);
    int rc = forward_install(url);
    const char *reply = (rc == 0) ? ok_resp : err_resp;
    send(cl, reply, strlen(reply), 0);
    close(cl);
    return NULL;
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(void) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if(srv < 0) return 1;

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sa = {0};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);   /* public-facing */
    sa.sin_port        = htons(DPIV2_PORT);

    /* Retry bind up to 6 times (5 s apart) in case the port is transiently
     * held by a system service that releases it quickly after boot. */
    int bound = 0;
    for (int attempt = 0; attempt < 6; attempt++) {
        if (attempt > 0) {
            fprintf(stderr, "[dpiv2] bind port %d busy, retry %d/5 in 5 s\n",
                    DPIV2_PORT, attempt);
            struct timespec ts = {5, 0};
            nanosleep(&ts, NULL);
        }
        if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
            bound = 1;
            break;
        }
    }
    if (!bound) {
        fprintf(stderr, "[dpiv2] bind port %d failed after retries\n", DPIV2_PORT);
        close(srv);
        return 1;
    }
    if(listen(srv, 8) < 0) { close(srv); return 1; }

    fprintf(stderr, "[dpiv2] listening on 0.0.0.0:%d\n", DPIV2_PORT);

    for(;;) {
        int cl = accept(srv, NULL, NULL);
        if(cl < 0) continue;

        conn_args_t *ca = malloc(sizeof(*ca));
        if(!ca) { close(cl); continue; }
        ca->fd = cl;

        pthread_t tid;
        if(pthread_create(&tid, NULL, handle_conn, ca) != 0) {
            free(ca);
            close(cl);
        } else {
            pthread_detach(tid);
        }
    }

    close(srv);
    return 0;
}
