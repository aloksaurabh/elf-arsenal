
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "elf2fself.h"

/* Globals that elf2fself.c references via extern decl in utils.h. */
char g_log_path[512] = {0};
int  g_enable_logging = 1;


#define JB_AUTHID  0x4801000000000013ULL

#define STATUS_PATH       "/data/elf-arsenal/last-sdk-changer.json"
#define SCAN_OUTPUT_PATH  "/data/elf-arsenal/sdk-scan.json"

/* ELF / SCE param magic — same values as ps5-app-dumper's
   ps5_backport.c. */
#define ELF_MAGIC               "\x7F""ELF"
#define PS4_FSELF_MAGIC         "\x4F\x15\x3D\x1D"
#define PS5_FSELF_MAGIC         "\x54\x14\xF5\xEE"
#define PT_SCE_PROCPARAM        0x61000001U
#define PT_SCE_MODULE_PARAM     0x61000002U
#define SCE_PROCESS_PARAM_MAGIC 0x4942524F
#define SCE_MODULE_PARAM_MAGIC  0x3C13F4BF
#define SCE_PARAM_PS5_SDK_OFFSET 0xC
#define SCE_PARAM_PS4_SDK_OFFSET 0x8
#define PHT_OFFSET_OFFSET   0x20
#define PHT_COUNT_OFFSET    0x38
#define PHDR_ENTRY_SIZE     0x38
#define PHDR_TYPE_OFFSET    0x00
#define PHDR_OFFSET_OFFSET  0x08


/* ── jb_escalate_pid (same primitive backup-helper uses) ──────────── */

static int
jb_escalate_pid(pid_t pid) {
    if (pid <= 0) return -1;
    intptr_t proc = kernel_get_proc(pid);
    if (!proc) return -1;
    int rc = 0;
    if (kernel_set_ucred_uid (pid, 0) != 0) rc = -1;
    if (kernel_set_ucred_ruid(pid, 0) != 0) rc = -1;
    if (kernel_set_ucred_svuid(pid, 0)!= 0) rc = -1;
    if (kernel_set_ucred_rgid(pid, 0) != 0) rc = -1;
    if (kernel_set_ucred_svgid(pid,0) != 0) rc = -1;
    intptr_t rootvnode = kernel_get_root_vnode();
    if (rootvnode) {
        if (kernel_set_proc_rootdir(pid, rootvnode) != 0) rc = -1;
        if (kernel_set_proc_jaildir(pid, rootvnode) != 0) rc = -1;
    }
    if (kernel_set_ucred_authid(pid, JB_AUTHID) != 0) rc = -1;
    uint8_t caps[16]; memset(caps, 0xff, sizeof(caps));
    if (kernel_set_ucred_caps(pid, caps) != 0) rc = -1;
    if (kernel_set_ucred_attrs(pid, 0x80) != 0) rc = -1;
    return rc;
}


/* ── status writing ──────────────────────────────────────────────── */

static void
write_status(const char *json) {
    mkdir("/data", 0755);
    mkdir("/data/elf-arsenal", 0755);
    int fd = open(STATUS_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    (void)write(fd, json, strlen(json));
    close(fd);
}

#define LOG(fmt, ...) do { \
    fprintf(stderr, "sdk-changer: " fmt "\n", ##__VA_ARGS__); \
} while (0)


/* ── param.json reader (minimal hand-rolled JSON scan) ───────────── */

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
    while (*p && *p != '"' && i < out_size - 1) {
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


/* ── param.sfo reader (PS4 / CUSA) ────────────────────────────────── */
/* Minimal SFO parser — pulls TITLE_ID, TITLE strings + extracts the
   `sdk_ver=` substring from PUBTOOLINFO. */

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
            size_t n = e[i].length;
            if (n >= tid_size) n = tid_size - 1;
            memcpy(title_id, d, n); title_id[n] = 0;
        } else if (title_name && !strcmp(k, "TITLE")) {
            size_t n = e[i].length;
            if (n >= tn_size) n = tn_size - 1;
            memcpy(title_name, d, n); title_name[n] = 0;
        } else if (sdk_ver_out && !strcmp(k, "PUBTOOLINFO")) {
            const char *s = strstr((const char *)d, "sdk_ver=");
            if (s) *sdk_ver_out = (uint32_t)strtoul(s + 8, NULL, 16);
        }
    }
    free(buf);
    return 0;
}


/* ── make .bak ────────────────────────────────────────────────────── */

static int
make_bak(const char *path) {
    char bak[1024];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    struct stat st;
    if (stat(bak, &st) == 0) return 0;     /* already exists, leave alone */

    int sfd = open(path, O_RDONLY);
    if (sfd < 0) return -1;
    int dfd = open(bak, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dfd < 0) { close(sfd); return -1; }
    char buf[64*1024]; ssize_t n;
    while ((n = read(sfd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(dfd, buf + off, n - off);
            if (w <= 0) { close(sfd); close(dfd); unlink(bak); return -1; }
            off += w;
        }
    }
    close(sfd); close(dfd);
    return 0;
}


/* Scan a file (raw ELF or fakeself) for the SCE_PROCESS_PARAM /
   SCE_MODULE_PARAM magic bytes and patch the SDK fields in-place.
   Works regardless of whether the file is raw ELF or fakeself-wrapped.
   Returns hit count, 0 if none found, -1 on I/O error. */
static int
patch_inplace_sdk(const char *path, uint32_t target_ps5, uint32_t target_ps4) {
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0x40) { close(fd); return -1; }
    uint8_t *map = mmap(NULL, st.st_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return -1; }

    int hits = 0;
    const uint32_t pp_magic = SCE_PROCESS_PARAM_MAGIC;
    const uint32_t mp_magic = SCE_MODULE_PARAM_MAGIC;

    for (off_t i = 0; i + 0x18 <= st.st_size; i += 4) {
        uint32_t v = *(uint32_t *)(map + i);
        if (v != pp_magic && v != mp_magic) continue;

        uint8_t *param = map + i;
        if (target_ps5) {
            uint32_t old = *(uint32_t *)(param + SCE_PARAM_PS5_SDK_OFFSET);
            *(uint32_t *)(param + SCE_PARAM_PS5_SDK_OFFSET) = target_ps5;
            LOG("  %s @0x%lx: PS5 SDK 0x%08X -> 0x%08X",
                path, (unsigned long)i, old, target_ps5);
        }
        if (target_ps4) {
            uint32_t old = *(uint32_t *)(param + SCE_PARAM_PS4_SDK_OFFSET);
            *(uint32_t *)(param + SCE_PARAM_PS4_SDK_OFFSET) = target_ps4;
            LOG("  %s @0x%lx: PS4 SDK 0x%08X -> 0x%08X",
                path, (unsigned long)i, old, target_ps4);
        }
        hits++;
        i += 0x40;
    }

    munmap(map, st.st_size);
    close(fd);
    return hits;
}


static int
patch_elf_sdk(const char *path, uint32_t target_ps5, uint32_t target_ps4) {
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0x40) { close(fd); return 1; }
    uint8_t *map = mmap(NULL, st.st_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return -1; }

    int patched = 0;

    /* fakeself headers — patching not supported here. */
    if (memcmp(map, PS4_FSELF_MAGIC, 4) == 0 ||
        memcmp(map, PS5_FSELF_MAGIC, 4) == 0) {
        munmap(map, st.st_size); close(fd);
        return 0;  /* signed/fself: caller should rebuild via elf2fself */
    }
    if (memcmp(map, ELF_MAGIC, 4) != 0) {
        munmap(map, st.st_size); close(fd); return 1;
    }

    uint64_t phoff = *(uint64_t *)(map + PHT_OFFSET_OFFSET);
    uint16_t phnum = *(uint16_t *)(map + PHT_COUNT_OFFSET);
    if (phoff + phnum * PHDR_ENTRY_SIZE > (uint64_t)st.st_size) {
        munmap(map, st.st_size); close(fd); return -1;
    }

    for (uint16_t i = 0; i < phnum; i++) {
        uint8_t *phdr = map + phoff + i * PHDR_ENTRY_SIZE;
        uint32_t p_type   = *(uint32_t *)(phdr + PHDR_TYPE_OFFSET);
        uint64_t p_offset = *(uint64_t *)(phdr + PHDR_OFFSET_OFFSET);
        if (p_type != PT_SCE_PROCPARAM && p_type != PT_SCE_MODULE_PARAM) continue;
        if (p_offset + 0x18 > (uint64_t)st.st_size) continue;

        uint8_t *param = map + p_offset;
        uint32_t magic = *(uint32_t *)param;
        if ((p_type == PT_SCE_PROCPARAM && magic != SCE_PROCESS_PARAM_MAGIC) ||
            (p_type == PT_SCE_MODULE_PARAM && magic != SCE_MODULE_PARAM_MAGIC)) {
            param += 8; magic = *(uint32_t *)param;
        }
        if ((p_type == PT_SCE_PROCPARAM && magic != SCE_PROCESS_PARAM_MAGIC) ||
            (p_type == PT_SCE_MODULE_PARAM && magic != SCE_MODULE_PARAM_MAGIC))
            continue;

        if (target_ps5 &&
            p_offset + SCE_PARAM_PS5_SDK_OFFSET + 4 <= (uint64_t)st.st_size) {
            uint32_t old = *(uint32_t *)(param + SCE_PARAM_PS5_SDK_OFFSET);
            *(uint32_t *)(param + SCE_PARAM_PS5_SDK_OFFSET) = target_ps5;
            LOG("  %s: PS5 SDK 0x%08X → 0x%08X", path, old, target_ps5);
            patched = 1;
        }
        if (target_ps4 &&
            p_offset + SCE_PARAM_PS4_SDK_OFFSET + 4 <= (uint64_t)st.st_size) {
            uint32_t old = *(uint32_t *)(param + SCE_PARAM_PS4_SDK_OFFSET);
            *(uint32_t *)(param + SCE_PARAM_PS4_SDK_OFFSET) = target_ps4;
            LOG("  %s: PS4 SDK 0x%08X → 0x%08X", path, old, target_ps4);
            patched = 1;
        }
    }

    munmap(map, st.st_size);
    close(fd);
    return patched ? 0 : 0;
}


/* ── param.json rewriter — patches sdkVersion + requiredSystemSoftwareVersion */

static int
patch_param_json(const char *path, uint32_t new_sdk) {
    if (!new_sdk) return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size > 1024*1024) { close(fd); return -1; }
    char *buf = malloc(st.st_size + 1);
    if (!buf) { close(fd); return -1; }
    ssize_t n = read(fd, buf, st.st_size);
    close(fd);
    if (n != st.st_size) { free(buf); return -1; }
    buf[st.st_size] = 0;

    /* param.json carries these as quoted 64-bit hex strings, e.g.
       `"sdkVersion": "0x0300000000000000"`. The 32-bit value we receive
       lives in the upper 32 bits of that 64-bit field. */
    uint64_t new64 = (uint64_t)new_sdk << 32;
    char val_str[32];
    snprintf(val_str, sizeof(val_str), "\"0x%016llx\"", (unsigned long long)new64);

    char *out = malloc(st.st_size + 1024);
    if (!out) { free(buf); return -1; }
    size_t out_len = 0;

    const char *keys[] = { "sdkVersion", "requiredSystemSoftwareVersion", NULL };
    const char *cursor = buf;
    int changed = 0;

    while (cursor < buf + st.st_size) {
        const char *next_match = NULL;
        const char *next_key   = NULL;
        for (int k = 0; keys[k]; k++) {
            char needle[64];
            snprintf(needle, sizeof(needle), "\"%s\"", keys[k]);
            const char *m = strstr(cursor, needle);
            if (m && (!next_match || m < next_match)) {
                next_match = m;
                next_key = keys[k];
            }
        }
        if (!next_match) break;

        const char *p = strchr(next_match, ':');
        if (!p) break;
        p++;
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;

        const char *val_start = p;
        if (*p == '"') {
            p++;
            while (*p && *p != '"') p++;
            if (*p == '"') p++;
        } else {
            while (*p && (*p == 'x' || *p == 'X' ||
                         (*p >= '0' && *p <= '9') ||
                         (*p >= 'a' && *p <= 'f') ||
                         (*p >= 'A' && *p <= 'F'))) p++;
        }
        const char *val_end = p;

        size_t pre = val_start - cursor;
        memcpy(out + out_len, cursor, pre); out_len += pre;
        size_t vlen = strlen(val_str);
        memcpy(out + out_len, val_str, vlen); out_len += vlen;
        LOG("  %s: %s %.*s → %s", path, next_key,
            (int)(val_end - val_start), val_start, val_str);
        changed = 1;
        cursor = val_end;
    }
    /* Copy tail. */
    size_t tail = (buf + st.st_size) - cursor;
    memcpy(out + out_len, cursor, tail); out_len += tail;

    if (changed) {
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            (void)write(fd, out, out_len);
            close(fd);
        }
    }
    free(buf); free(out);
    return changed ? 0 : 1;
}


/* ── walk an opaque directory for executables, calling cb on each ─── */

static int
ends_with_ci(const char *s, const char *suffix) {
    size_t sl = strlen(s), tl = strlen(suffix);
    if (tl > sl) return 0;
    return strcasecmp(s + sl - tl, suffix) == 0;
}

static int
is_executable_name(const char *fname) {
    if (!strcmp(fname, "eboot.bin")) return 1;
    if (ends_with_ci(fname, ".self")) return 1;
    if (ends_with_ci(fname, ".sprx")) return 1;
    if (ends_with_ci(fname, ".prx")) return 1;
    if (ends_with_ci(fname, ".elf")) return 1;
    return 0;
}

typedef int (*file_cb_t)(const char *path, void *user);

static int
walk_for_files(const char *root, file_cb_t cb, void *user, int depth) {
    if (depth > 8) return 0;
    DIR *d = opendir(root);
    if (!d) return 0;
    struct dirent *e;
    int count = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[1024];
        if (snprintf(child, sizeof(child), "%s/%s", root, e->d_name) >= (int)sizeof(child)) continue;
        struct stat st;
        if (lstat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            count += walk_for_files(child, cb, user, depth + 1);
        } else if (S_ISREG(st.st_mode) && is_executable_name(e->d_name)) {
            if (cb(child, user) == 0) count++;
        }
    }
    closedir(d);
    return count;
}


/* ── op: scan ─────────────────────────────────────────────────────── */

/* SMP-style default scan roots. */
static const char *SCAN_ROOTS[] = {
    "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
    "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7",
    "/mnt/ext0", "/mnt/ext1", "/mnt/ext2", "/mnt/ext3",
    "/data",
    NULL,
};

typedef struct {
    FILE *fp;
    int first;
} scan_ctx_t;

/* Emit one game entry. */
static void
emit_game(scan_ctx_t *sc, const char *path,
          const char *title_id, const char *title_name,
          uint32_t ps5_sdk, uint32_t ps4_sdk, int is_ps4) {
    if (!sc->first) fputs(",", sc->fp);
    sc->first = 0;
    /* Escape just the obvious JSON-dangerous chars in title_name. */
    char escaped[256] = {0};
    size_t j = 0;
    for (size_t i = 0; title_name[i] && j < sizeof(escaped) - 2; i++) {
        char c = title_name[i];
        if (c == '"' || c == '\\') escaped[j++] = '\\';
        if (c < 0x20) c = ' ';
        escaped[j++] = c;
    }
    fprintf(sc->fp,
        "{\"path\":\"%s\","
         "\"titleId\":\"%s\","
         "\"titleName\":\"%s\","
         "\"sdkPs5\":%u,"
         "\"sdkPs4\":%u,"
         "\"isPs4\":%s}",
        path, title_id, escaped, ps5_sdk, ps4_sdk,
        is_ps4 ? "true" : "false");
    LOG("  game: %s [%s] sdk_ps5=0x%08x sdk_ps4=0x%08x %s",
        title_id, escaped, ps5_sdk, ps4_sdk, path);
}

/* Try to identify `dir` as a game folder. Returns 1 if processed. */
static int
maybe_process_game_dir(const char *dir, scan_ctx_t *sc) {
    char p[1024];
    /* PS5 PPSA */
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
        if (json_extract_str(buf, "masterVersion", NULL, 0) != 0) { /* unused */ }
        json_extract_str(buf, "titleName", title_name, sizeof(title_name));
        if (!title_name[0])
            json_extract_str(buf, "title", title_name, sizeof(title_name));
        json_extract_num(buf, "sdkVersion", &sdk_v);
        emit_game(sc, dir, title_id, title_name,
                  (uint32_t)sdk_v, 0, 0);
        return 1;
    }
    /* PS4 CUSA */
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
    if (maybe_process_game_dir(root, sc)) return;  /* don't descend into a game */
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (e->d_name[0] == '.') continue;
        char child[1024];
        if (snprintf(child, sizeof(child), "%s/%s", root, e->d_name) >= (int)sizeof(child)) continue;
        struct stat st;
        if (lstat(child, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        scan_recursive(child, sc, depth + 1);
    }
    closedir(d);
}

static int
op_scan(void) {
    write_status("{\"ok\":true,\"pending\":true,\"kind\":\"scan\"}");

    mkdir("/data/elf-arsenal", 0755);
    FILE *fp = fopen(SCAN_OUTPUT_PATH, "w");
    if (!fp) {
        write_status("{\"ok\":false,\"finished\":true,\"kind\":\"scan\","
                     "\"error\":\"cannot open output\"}");
        return 1;
    }
    fputs("{\"games\":[", fp);
    scan_ctx_t sc = { .fp = fp, .first = 1 };

    for (int i = 0; SCAN_ROOTS[i]; i++) {
        struct stat st;
        if (stat(SCAN_ROOTS[i], &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        LOG("scanning %s ...", SCAN_ROOTS[i]);
        scan_recursive(SCAN_ROOTS[i], &sc, 0);
    }
    fprintf(fp, "],\"finishedAt\":%lld}", (long long)time(NULL));
    fclose(fp);

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"finished\":true,\"kind\":\"scan\","
         "\"output\":\"%s\",\"finishedAt\":%lld}",
        SCAN_OUTPUT_PATH, (long long)time(NULL));
    write_status(buf);
    LOG("scan complete → %s", SCAN_OUTPUT_PATH);
    return 0;
}


/* ── op: apply ────────────────────────────────────────────────────── */

typedef struct {
    uint32_t ps5; uint32_t ps4;
    int patched; int failed; int skipped_already_fself;
} apply_ctx_t;

static int
apply_one(const char *path, void *user) {
    apply_ctx_t *ac = user;

    if (make_bak(path) < 0) {
        LOG("  %s: bak failed", path);
        ac->failed++;
        return -1;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) { ac->failed++; return -1; }
    uint8_t magic[4]; ssize_t n = read(fd, magic, 4);
    close(fd);
    if (n != 4) { ac->failed++; return -1; }

    int is_fself = (memcmp(magic, PS4_FSELF_MAGIC, 4) == 0 ||
                    memcmp(magic, PS5_FSELF_MAGIC, 4) == 0);
    int is_elf   = (memcmp(magic, ELF_MAGIC, 4) == 0);
    if (!is_fself && !is_elf) return 0;

    int hits = patch_inplace_sdk(path, ac->ps5, ac->ps4);
    if (hits < 0) {
        LOG("  %s: in-place patch failed (I/O)", path);
        ac->failed++;
        return -1;
    }
    if (hits == 0) {
        LOG("  %s: no procparam magic found (nothing to patch)", path);
        ac->skipped_already_fself++;
        return 0;
    }
    LOG("  %s: patched %d procparam site(s)", path, hits);
    ac->patched++;
    return 0;
}

static int
op_apply(const char *game_dir, uint32_t ps5_sdk, uint32_t ps4_sdk) {
    char start[256];
    snprintf(start, sizeof(start),
        "{\"ok\":true,\"pending\":true,\"kind\":\"apply\","
         "\"path\":\"%s\"}", game_dir);
    write_status(start);

    LOG("apply on %s   target ps5=0x%08x ps4=0x%08x", game_dir, ps5_sdk, ps4_sdk);

    apply_ctx_t ac = { .ps5 = ps5_sdk, .ps4 = ps4_sdk };
    int total = walk_for_files(game_dir, apply_one, &ac, 0);
    (void)total;

    /* Also patch param.json. */
    char pj[1024];
    snprintf(pj, sizeof(pj), "%s/sce_sys/param.json", game_dir);
    if (access(pj, R_OK) == 0) {
        if (make_bak(pj) == 0) patch_param_json(pj, ps5_sdk);
    }

    char done[512];
    snprintf(done, sizeof(done),
        "{\"ok\":%s,\"finished\":true,\"kind\":\"apply\","
         "\"path\":\"%s\",\"patched\":%d,\"failed\":%d,"
         "\"skippedFself\":%d,\"finishedAt\":%lld}",
        ac.failed == 0 ? "true" : "false",
        game_dir, ac.patched, ac.failed, ac.skipped_already_fself,
        (long long)time(NULL));
    write_status(done);
    LOG("apply done: patched=%d failed=%d skipped_fself=%d",
        ac.patched, ac.failed, ac.skipped_already_fself);
    return ac.failed > 0 ? 1 : 0;
}


/* ── entry ───────────────────────────────────────────────────────── */

int
main(int argc, char **argv) {
    jb_escalate_pid(getpid());

    LOG("starting pid=%d argc=%d", getpid(), argc);
    for (int i = 0; i < argc; i++) LOG("  argv[%d]=%s", i, argv[i]);

    if (argc < 2) {
        LOG("usage: sdk-changer scan");
        LOG("       sdk-changer apply <game_dir> <ps5_sdk_hex> <ps4_sdk_hex>");
        write_status("{\"ok\":false,\"finished\":true,\"error\":\"missing op\"}");
        return 2;
    }

    const char *op = argv[1];
    if (!strcmp(op, "scan")) return op_scan();
    if (!strcmp(op, "apply")) {
        if (argc < 5) {
            LOG("apply needs <game_dir> <ps5_sdk_hex> <ps4_sdk_hex>");
            write_status("{\"ok\":false,\"finished\":true,"
                         "\"error\":\"apply needs game_dir + ps5_sdk + ps4_sdk\"}");
            return 2;
        }
        uint32_t ps5 = (uint32_t)strtoul(argv[3], NULL, 0);
        uint32_t ps4 = (uint32_t)strtoul(argv[4], NULL, 0);
        return op_apply(argv[2], ps5, ps4);
    }
    LOG("unknown op '%s'", op);
    return 2;
}
