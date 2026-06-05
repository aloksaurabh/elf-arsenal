
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>


#define BACKUPS_ROOT      "/data/elf-arsenal/backups"
#define STATUS_PATH       "/data/elf-arsenal/last-backup-job.json"
#define JB_AUTHID         0x4801000000000013ULL


/* ── jb_escalate_pid (same primitive elf-arsenal uses) ───────────── */

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
    uint8_t caps[16];
    memset(caps, 0xff, sizeof(caps));
    if (kernel_set_ucred_caps(pid, caps) != 0) rc = -1;
    if (kernel_set_ucred_attrs(pid, 0x80) != 0) rc = -1;
    return rc;
}


/* ── status file writes ──────────────────────────────────────────── */

static void
write_status(const char *json) {
    mkdir("/data", 0755);
    mkdir("/data/elf-arsenal", 0755);
    int fd = open(STATUS_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    (void)write(fd, json, strlen(json));
    close(fd);
}

static void
status_running(const char *kind, const char *tag, const char *ts) {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"running\":true,\"kind\":\"%s\","
        "\"tag\":\"%s\",\"ts\":\"%s\","
        "\"startedAt\":%lld}",
        kind, tag ? tag : "", ts ? ts : "",
        (long long)time(NULL));
    write_status(buf);
}

static void
status_finished(const char *kind, int ok, const char *tag,
                const char *ts, int restored_count) {
    char buf[640];
    if (restored_count >= 0) {
        snprintf(buf, sizeof(buf),
            "{\"ok\":%s,\"finished\":true,\"kind\":\"%s\","
            "\"tag\":\"%s\",\"ts\":\"%s\","
            "\"restored\":%d,\"finishedAt\":%lld}",
            ok ? "true" : "false", kind,
            tag ? tag : "", ts ? ts : "",
            restored_count,
            (long long)time(NULL));
    } else {
        snprintf(buf, sizeof(buf),
            "{\"ok\":%s,\"finished\":true,\"kind\":\"%s\","
            "\"tag\":\"%s\",\"ts\":\"%s\","
            "\"finishedAt\":%lld}",
            ok ? "true" : "false", kind,
            tag ? tag : "", ts ? ts : "",
            (long long)time(NULL));
    }
    write_status(buf);
}


/* ── filesystem primitives ───────────────────────────────────────── */

static int
mkpath_p(const char *path) {
    char tmp[1024];
    size_t len = strnlen(path, sizeof(tmp));
    if (len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    return mkdir(tmp, 0755);
}

static int
copy_file(const char *src, const char *dst) {
    int sfd = open(src, O_RDONLY);
    if (sfd < 0) {
        fprintf(stderr, "bhelper copy: open(src=%s) errno=%d\n", src, errno);
        return -1;
    }
    struct stat st;
    if (fstat(sfd, &st) != 0) {
        fprintf(stderr, "bhelper copy: fstat(%s) errno=%d\n", src, errno);
        close(sfd);
        return -1;
    }
    char parent[1024];
    snprintf(parent, sizeof(parent), "%s", dst);
    char *slash = strrchr(parent, '/');
    if (slash) { *slash = 0; mkpath_p(parent); }
    int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dfd < 0) {
        fprintf(stderr, "bhelper copy: open(dst=%s) errno=%d\n", dst, errno);
        close(sfd);
        return -1;
    }
    char buf[64 * 1024];
    ssize_t n;
    while ((n = read(sfd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(dfd, buf + off, n - off);
            if (w <= 0) {
                fprintf(stderr,
                    "bhelper copy: write(%s) errno=%d\n", dst, errno);
                close(sfd); close(dfd);
                unlink(dst);
                return -1;
            }
            off += w;
        }
    }
    fsync(dfd);
    close(sfd);
    close(dfd);
    return 0;
}

static int
rm_rf(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") ||
                    !strcmp(e->d_name, "..")) continue;
                char child[1024];
                snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
                rm_rf(child);
            }
            closedir(d);
        }
        return rmdir(path);
    }
    return unlink(path);
}


/* ── snapshot dir + manifest helpers ─────────────────────────────── */

static void
flatten_path(const char *src, char *out, size_t out_size) {
    const char *p = src;
    while (*p == '/') p++;
    size_t i = 0;
    while (*p && i < out_size - 1) {
        out[i++] = (*p == '/') ? '_' : *p;
        p++;
    }
    out[i] = 0;
}

static void
manifest_append(const char *snap_dir, const char *flat,
                const char *original) {
    char mpath[1024];
    snprintf(mpath, sizeof(mpath), "%s/.manifest", snap_dir);
    FILE *f = fopen(mpath, "a");
    if (!f) return;
    fprintf(f, "%s\t%s\n", flat, original);
    fclose(f);
}


/* ── operations ──────────────────────────────────────────────────── */

static int
op_dump_registry(void) {
    status_running("dump-registry", NULL, NULL);

    char snapdir[1024];
    snprintf(snapdir, sizeof(snapdir),
             "%s/registry-full/%ld", BACKUPS_ROOT, (long)time(NULL));
    mkpath_p(snapdir);

    static const char *targets[] = {
        "/system_data/priv/mms/app.db",
        "/system_data/priv/mms/appinfo.db",
        "/system_data/priv/mms/addcont.db",
        "/system_data/priv/mms/addcont_backup.db",
        "/system_data/priv/mms/notification2.db",
        "/system_data/priv/mms/bgft.db",
        NULL,
    };
    int total = 0;
    for (int i = 0; targets[i]; i++) {
        char flat[512];
        flatten_path(targets[i], flat, sizeof(flat));
        char dst[1024];
        snprintf(dst, sizeof(dst), "%s/%s", snapdir, flat);
        if (copy_file(targets[i], dst) == 0) {
            manifest_append(snapdir, flat, targets[i]);
            total++;
            fprintf(stderr, "bhelper: dumped %s\n", targets[i]);
        }
    }

    /* Sweep any reg* file. */
    DIR *d = opendir("/system_data/priv/mms");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strncmp(e->d_name, "reg", 3) != 0 &&
                strncmp(e->d_name, "registry", 8) != 0) continue;
            char src[256];
            snprintf(src, sizeof(src),
                     "/system_data/priv/mms/%s", e->d_name);
            char flat[512];
            flatten_path(src, flat, sizeof(flat));
            char dst[1024];
            snprintf(dst, sizeof(dst), "%s/%s", snapdir, flat);
            if (copy_file(src, dst) == 0) {
                manifest_append(snapdir, flat, src);
                total++;
            }
        }
        closedir(d);
    }

    fprintf(stderr, "bhelper: dump-registry total=%d\n", total);
    status_finished("dump-registry", total > 0, NULL, NULL, -1);
    return total > 0 ? 0 : 1;
}

static int
op_restore(const char *tag, const char *ts) {
    status_running("restore", tag, ts);
    char snapdir[768];
    snprintf(snapdir, sizeof(snapdir),
             "%s/%s/%s", BACKUPS_ROOT, tag, ts);

    char mpath[1024];
    snprintf(mpath, sizeof(mpath), "%s/.manifest", snapdir);
    FILE *f = fopen(mpath, "r");
    if (!f) {
        fprintf(stderr,
            "bhelper: restore: manifest %s missing\n", mpath);
        status_finished("restore", 0, tag, ts, 0);
        return 1;
    }
    int n = 0;
    char line[1100];
    while (fgets(line, sizeof(line), f)) {
        size_t L = strlen(line);
        while (L > 0 && (line[L-1] == '\n' || line[L-1] == '\r'))
            line[--L] = 0;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        const char *flat = line;
        const char *original = tab + 1;
        char src[1024];
        snprintf(src, sizeof(src), "%s/%s", snapdir, flat);
        if (copy_file(src, original) == 0) {
            n++;
            fprintf(stderr,
                "bhelper: restored %s -> %s\n", src, original);
        }
    }
    fclose(f);
    status_finished("restore", n >= 0, tag, ts, n);
    return n > 0 ? 0 : 1;
}

static int
op_profile(const char *uid_hex) {
    status_running("profile", uid_hex, NULL);

    char uid_upper[24];
    size_t L = strlen(uid_hex);
    if (L >= sizeof(uid_upper) - 1) L = sizeof(uid_upper) - 2;
    size_t i;
    for (i = 0; i < L; i++) {
        char c = uid_hex[i];
        uid_upper[i] = (c >= 'a' && c <= 'f') ? (c - 32) : c;
    }
    uid_upper[i] = 0;

    char snapdir[1024];
    snprintf(snapdir, sizeof(snapdir),
             "%s/profile/%ld", BACKUPS_ROOT, (long)time(NULL));
    mkpath_p(snapdir);

    int total = 0;

    /* Per-user single-file targets. */
    char path[256];
    snprintf(path, sizeof(path),
             "/system_data/priv/home/%s/config.dat", uid_hex);
    {
        char flat[512]; flatten_path(path, flat, sizeof(flat));
        char dst[1024]; snprintf(dst, sizeof(dst), "%s/%s", snapdir, flat);
        if (copy_file(path, dst) == 0) {
            manifest_append(snapdir, flat, path);
            total++;
            fprintf(stderr, "bhelper: profile: %s\n", path);
        }
    }
    snprintf(path, sizeof(path),
             "/system_data/priv/home/%s/np/auth.dat", uid_hex);
    {
        char flat[512]; flatten_path(path, flat, sizeof(flat));
        char dst[1024]; snprintf(dst, sizeof(dst), "%s/%s", snapdir, flat);
        if (copy_file(path, dst) == 0) {
            manifest_append(snapdir, flat, path);
            total++;
            fprintf(stderr, "bhelper: profile: %s\n", path);
        }
    }

    /* Profile cache dir: walk + capture every regular file. */
    char cache_dir[256];
    snprintf(cache_dir, sizeof(cache_dir),
             "/system_data/priv/cache/profile/0x%s", uid_upper);
    DIR *d = opendir(cache_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") ||
                !strcmp(e->d_name, "..")) continue;
            char src[512];
            snprintf(src, sizeof(src), "%s/%s", cache_dir, e->d_name);
            struct stat st;
            if (lstat(src, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            char flat[512]; flatten_path(src, flat, sizeof(flat));
            char dst[1024]; snprintf(dst, sizeof(dst), "%s/%s", snapdir, flat);
            if (copy_file(src, dst) == 0) {
                manifest_append(snapdir, flat, src);
                total++;
            }
        }
        closedir(d);
        fprintf(stderr, "bhelper: profile: cache dir %s walked\n",
                cache_dir);
    } else {
        fprintf(stderr, "bhelper: profile: cache dir %s missing\n",
                cache_dir);
    }

    fprintf(stderr, "bhelper: profile total=%d files\n", total);
    status_finished("profile", total > 0, uid_hex, NULL, -1);
    return total > 0 ? 0 : 1;
}

static int
op_trophies(const char *uid_hex) {
    status_running("trophies", uid_hex, NULL);

    char snapdir[1024];
    snprintf(snapdir, sizeof(snapdir),
             "%s/trophies/%ld", BACKUPS_ROOT, (long)time(NULL));
    mkpath_p(snapdir);

    int total = 0;

    /* Walk both v1 and v2 trophy roots and capture every regular file
       at one nested level (NPWR/NPCommId dirs). */
    const char *roots[2];
    char p1[256], p2[256];
    snprintf(p1, sizeof(p1), "/user/home/%s/trophy2/nobackup/data", uid_hex);
    snprintf(p2, sizeof(p2), "/user/home/%s/trophy/data", uid_hex);
    roots[0] = p1;
    roots[1] = p2;

    for (int r = 0; r < 2; r++) {
        DIR *d = opendir(roots[r]);
        if (!d) { fprintf(stderr,
                          "bhelper: trophies: %s missing\n", roots[r]); continue; }
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") ||
                !strcmp(e->d_name, "..")) continue;
            char child[768];
            snprintf(child, sizeof(child), "%s/%s", roots[r], e->d_name);
            struct stat st;
            if (lstat(child, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            DIR *cd = opendir(child);
            if (!cd) continue;
            struct dirent *ce;
            while ((ce = readdir(cd))) {
                if (!strcmp(ce->d_name, ".") ||
                    !strcmp(ce->d_name, "..")) continue;
                char src[1024];
                snprintf(src, sizeof(src), "%s/%s", child, ce->d_name);
                struct stat fst;
                if (lstat(src, &fst) != 0 || !S_ISREG(fst.st_mode)) continue;
                char flat[512]; flatten_path(src, flat, sizeof(flat));
                char dst[2048]; snprintf(dst, sizeof(dst), "%s/%s", snapdir, flat);
                if (copy_file(src, dst) == 0) {
                    manifest_append(snapdir, flat, src);
                    total++;
                }
            }
            closedir(cd);
        }
        closedir(d);
    }

    fprintf(stderr, "bhelper: trophies total=%d files\n", total);
    status_finished("trophies", total > 0, uid_hex, NULL, -1);
    return total > 0 ? 0 : 1;
}

static int
op_delete(const char *tag, const char *ts) {
    status_running("delete", tag, ts);
    char snapdir[768];
    snprintf(snapdir, sizeof(snapdir),
             "%s/%s/%s", BACKUPS_ROOT, tag, ts);
    int rc = rm_rf(snapdir);
    /* Drop tag dir if empty. */
    char tagdir[640];
    snprintf(tagdir, sizeof(tagdir), "%s/%s", BACKUPS_ROOT, tag);
    rmdir(tagdir);
    fprintf(stderr,
        "bhelper: delete %s rc=%d\n", snapdir, rc);
    status_finished("delete", rc == 0, tag, ts, -1);
    return rc;
}


/* ── entry ───────────────────────────────────────────────────────── */

int
main(int argc, char **argv) {
    jb_escalate_pid(getpid());

    fprintf(stderr, "bhelper: starting pid=%d argc=%d\n",
            getpid(), argc);
    for (int i = 0; i < argc; i++) {
        fprintf(stderr, "bhelper: argv[%d]=%s\n", i, argv[i]);
    }

    if (argc < 2) {
        fprintf(stderr, "bhelper: missing operation argv\n");
        status_finished("none", 0, NULL, NULL, -1);
        return 2;
    }

    const char *op = argv[1];
    if (!strcmp(op, "dump-registry")) {
        return op_dump_registry();
    }
    if (!strcmp(op, "restore")) {
        if (argc < 4) {
            fprintf(stderr,
                "bhelper: restore needs <tag> <ts>\n");
            status_finished("restore", 0, NULL, NULL, 0);
            return 2;
        }
        return op_restore(argv[2], argv[3]);
    }
    if (!strcmp(op, "profile")) {
        if (argc < 3) {
            fprintf(stderr,
                "bhelper: profile needs <uid_hex>\n");
            status_finished("profile", 0, NULL, NULL, -1);
            return 2;
        }
        return op_profile(argv[2]);
    }
    if (!strcmp(op, "trophies")) {
        if (argc < 3) {
            fprintf(stderr,
                "bhelper: trophies needs <uid_hex>\n");
            status_finished("trophies", 0, NULL, NULL, -1);
            return 2;
        }
        return op_trophies(argv[2]);
    }
    if (!strcmp(op, "delete")) {
        if (argc < 4) {
            fprintf(stderr,
                "bhelper: delete needs <tag> <ts>\n");
            status_finished("delete", 0, NULL, NULL, -1);
            return 2;
        }
        return op_delete(argv[2], argv[3]);
    }
    fprintf(stderr, "bhelper: unknown op '%s'\n", op);
    status_finished(op, 0, NULL, NULL, -1);
    return 2;
}
