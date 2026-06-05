/* Elf Arsenal — automatic snapshot + restore. See backup.h. */

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
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <microhttpd.h>

#include "backup.h"
#include "sys.h"
#include "third_party/cJSON.h"
#include "websrv.h"


#define BACKUPS_ROOT      "/data/elf-arsenal/backups"
#define BACKUPS_INIT_FLAG "/data/elf-arsenal/backups/.initialized_v1"
#define BACKUP_JOB_STATUS_FILE "/data/elf-arsenal/last-backup-job.json"

#define BACKUPS_KEEP_PER_TAG 5


typedef struct {
    const char *tag;
    const char *label;
    const char *description;
} tag_meta_t;

static const tag_meta_t TAG_META[] = {
    { "initial",
      "Pristine first boot",
      "Console state the very first time Elf Arsenal ran on this box. "
      "Use this if you want to roll back EVERYTHING Elf Arsenal has "
      "done. Taken once, never overwritten." },
    { "registry-full",
      "Manual registry dump",
      "Full /system_data/priv/mms/ capture from the 📸 Dump button. "
      "Use as a safety anchor before risky operations." },
    { "trophy-all",
      "Before trophy-unlocker daemon",
      "app.db + appinfo.db captured at every boot, before the "
      "trophy-unlocker-all daemon patches them." },
    { "np-fake-signin",
      "Before NP fake sign-in",
      "config.dat captured before np-fake-signin modified your NP "
      "user state. Restore here to undo a fake-signin." },
    { "np-restore-account",
      "Before NP account restore",
      "config.dat captured before np-restore-account rewrote the "
      "registry from your existing config." },
    { "autoload-migrate",
      "Before autoloader migration",
      "Captured before sonic-loader.elf → elf-arsenal.elf rename "
      "sweep modified autoload.txt entries on attached storage." },
    { "profile",
      "User profile (config.dat + avatar)",
      "On-demand snapshot of the active user's NP profile (config.dat + "
      "auth.dat) plus the full avatar/profile-cache (PNGs, .dds icons, "
      "online.json). Restore it later, then run NP restore from this "
      "Settings panel so SCE re-reads the registry from the restored "
      "config.dat." },
    { "trophies",
      "User trophies (per-game)",
      "On-demand snapshot of the active user's trophy data for every "
      "game (trophy.img, sealedkey, trpsummary.dat for v1; TRPTITLE.DAT "
      "for v2 / NPCommId). Restore on the SAME console works cleanly "
      "since accountId is unchanged. Cross-console restore needs matching "
      "local-user names." },
    { NULL, NULL, NULL }
};

static const tag_meta_t *
tag_meta_lookup(const char *tag) {
    for (int i = 0; TAG_META[i].tag; i++) {
        if (!strcmp(TAG_META[i].tag, tag)) return &TAG_META[i];
    }
    return NULL;
}

static pthread_mutex_t g_backup_lock = PTHREAD_MUTEX_INITIALIZER;


static int g_backup_enabled = 1;


int
backup_is_enabled(void) {
    return g_backup_enabled;
}


void
backup_set_enabled(int on) {
    g_backup_enabled = on ? 1 : 0;
}


/* ── helpers ─────────────────────────────────────────────────────── */

static int
mkpath_p(const char *path) {
    /* mkdir -p, in-place rewrite of `tmp`. */
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
        fprintf(stderr, "backup copy_file: open(src) %s failed: %s\n",
                src, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(sfd, &st) != 0) {
        fprintf(stderr, "backup copy_file: fstat(%s) failed: %s\n",
                src, strerror(errno));
        close(sfd);
        return -1;
    }
    /* Make sure dest dir exists. */
    char parent[1024];
    snprintf(parent, sizeof(parent), "%s", dst);
    char *slash = strrchr(parent, '/');
    if (slash) { *slash = 0; mkpath_p(parent); }
    int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dfd < 0) {
        fprintf(stderr, "backup copy_file: open(dst) %s failed: %s\n",
                dst, strerror(errno));
        close(sfd);
        return -1;
    }
    char buf[64 * 1024];
    ssize_t n;
    while ((n = read(sfd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(dfd, buf + off, n - off);
            if (w <= 0) { close(sfd); close(dfd); unlink(dst); return -1; }
            off += w;
        }
    }
    fsync(dfd);
    close(sfd);
    close(dfd);
    /* Preserve mtime on the backup so audits make sense. */
    struct timespec times[2];
    times[0].tv_sec = st.st_atime; times[0].tv_nsec = 0;
    times[1].tv_sec = st.st_mtime; times[1].tv_nsec = 0;
    utimensat(AT_FDCWD, dst, times, 0);
    return 0;
}


static const char *
snapshot_dir(const char *tag) {
    static char path[1024];
    static char last_tag[64] = "";
    static time_t last_ts = 0;
    time_t now = time(NULL);
    /* Same tag + same second → reuse. */
    if (last_ts != 0 && now - last_ts < 1 && !strcmp(last_tag, tag)) {
        return path;
    }
    snprintf(path, sizeof(path), BACKUPS_ROOT "/%s/%ld", tag, (long)now);
    mkpath_p(path);
    strncpy(last_tag, tag, sizeof(last_tag) - 1);
    last_tag[sizeof(last_tag) - 1] = 0;
    last_ts = now;
    return path;
}


/* Append a manifest entry "<basename-in-snapshot>\t<original-path>" so
   restore knows where to put each file back. */
static void
manifest_append(const char *snap_dir, const char *snap_basename,
                const char *original) {
    char mpath[1024];
    snprintf(mpath, sizeof(mpath), "%s/.manifest", snap_dir);
    FILE *f = fopen(mpath, "a");
    if (!f) return;
    fprintf(f, "%s\t%s\n", snap_basename, original);
    fclose(f);
}


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


/* ── public API ──────────────────────────────────────────────────── */

int
backup_snapshot_file(const char *tag, const char *src_path) {
    if (!g_backup_enabled) return 0;                    /* disabled: silent no-op */
    struct stat st;
    if (stat(src_path, &st) != 0) return -1;            /* doesn't exist */
    if (!S_ISREG(st.st_mode)) return -1;                /* not a regular file */

    pthread_mutex_lock(&g_backup_lock);
    const char *snap = snapshot_dir(tag);
    char flat[512];
    flatten_path(src_path, flat, sizeof(flat));
    char dst[1024];
    snprintf(dst, sizeof(dst), "%s/%s", snap, flat);
    int rc = copy_file(src_path, dst);
    if (rc == 0) {
        manifest_append(snap, flat, src_path);
        fprintf(stderr, "backup[%s]: %s -> %s\n", tag, src_path, dst);
    } else {
        fprintf(stderr, "backup[%s]: COPY FAILED %s -> %s\n",
                tag, src_path, dst);
    }
    pthread_mutex_unlock(&g_backup_lock);
    return rc;
}


/* Recursive snapshot — used for /user/app/<tid>/ trees etc. Walks the
   src tree and snapshots every regular file it finds. */
static int
snapshot_tree_inner(const char *tag, const char *src, const char *snap_dir,
                    int depth) {
    if (depth > 8) return 0;     /* sanity cap */
    DIR *d = opendir(src);
    if (!d) return -1;
    struct dirent *e;
    int count = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[1024];
        if (snprintf(child, sizeof(child), "%s/%s", src, e->d_name) >= (int)sizeof(child)) continue;
        struct stat st;
        if (lstat(child, &st) != 0) continue;
        if (S_ISLNK(st.st_mode)) continue;
        if (S_ISDIR(st.st_mode)) {
            count += snapshot_tree_inner(tag, child, snap_dir, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            char flat[512];
            flatten_path(child, flat, sizeof(flat));
            char dst[1024];
            snprintf(dst, sizeof(dst), "%s/%s", snap_dir, flat);
            if (copy_file(child, dst) == 0) {
                manifest_append(snap_dir, flat, child);
                count++;
            }
        }
    }
    closedir(d);
    return count;
}

int
backup_snapshot_tree(const char *tag, const char *src_dir) {
    if (!g_backup_enabled) return 0;                    /* disabled: silent no-op */
    struct stat st;
    if (stat(src_dir, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    pthread_mutex_lock(&g_backup_lock);
    const char *snap = snapshot_dir(tag);
    int n = snapshot_tree_inner(tag, src_dir, snap, 0);
    fprintf(stderr, "backup[%s]: snapshot tree %s (%d files)\n",
            tag, src_dir, n);
    pthread_mutex_unlock(&g_backup_lock);
    return n;
}


static void
dump_registry_work_inner(void) {
    static const char *targets[] = {
        "/system_data/priv/mms/app.db",
        "/system_data/priv/mms/appinfo.db",
        "/system_data/priv/mms/addcont.db",
        "/system_data/priv/mms/addcont_backup.db",
        "/system_data/priv/mms/notification2.db",
        "/system_data/priv/mms/bgft.db",
        /* Registry files — names vary by FW. The snapshot tree walk
           below catches everything still in the dir. */
        NULL,
    };
    int total = 0;
    for (int i = 0; targets[i]; i++) {
        if (backup_snapshot_file("registry-full", targets[i]) == 0) total++;
    }
    pthread_mutex_lock(&g_backup_lock);
    const char *snap = snapshot_dir("registry-full");
    DIR *d = opendir("/system_data/priv/mms");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strncmp(e->d_name, "reg", 3) ||
                !strncmp(e->d_name, "registry", 8)) {
                char src[256];
                snprintf(src, sizeof(src),
                         "/system_data/priv/mms/%s", e->d_name);
                char flat[512];
                flatten_path(src, flat, sizeof(flat));
                char dst[1024];
                snprintf(dst, sizeof(dst), "%s/%s", snap, flat);
                if (copy_file(src, dst) == 0) {
                    manifest_append(snap, flat, src);
                    total++;
                }
            }
        }
        closedir(d);
    }
    pthread_mutex_unlock(&g_backup_lock);
    fprintf(stderr, "backup_dump_registry: %d files snapshotted\n", total);
}


/* Forward decl — defined after the worker-thread plumbing below. */
static int bj_enqueue_dump_registry(void);

int
backup_dump_registry(void) {
    if (!g_backup_enabled) return 0;                    /* disabled: silent no-op */
    return bj_enqueue_dump_registry();
}


static int
prune_empty_snapshots(void) {
    DIR *root = opendir(BACKUPS_ROOT);
    if (!root) return 0;
    int pruned = 0;
    struct dirent *te;
    while ((te = readdir(root))) {
        if (te->d_name[0] == '.') continue;
        char tagdir[768];
        snprintf(tagdir, sizeof(tagdir), "%s/%s", BACKUPS_ROOT, te->d_name);
        struct stat st;
        if (stat(tagdir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        DIR *td = opendir(tagdir);
        if (!td) continue;
        struct dirent *se;
        while ((se = readdir(td))) {
            if (se->d_name[0] == '.') continue;
            char snapdir[1024];
            snprintf(snapdir, sizeof(snapdir), "%s/%s", tagdir, se->d_name);
            if (stat(snapdir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

            /* Count regular files (excluding .manifest). */
            int files = 0;
            int manifest_lines = 0;
            DIR *sd = opendir(snapdir);
            if (!sd) continue;
            struct dirent *fe;
            while ((fe = readdir(sd))) {
                if (!strcmp(fe->d_name, ".") ||
                    !strcmp(fe->d_name, "..")) continue;
                if (!strcmp(fe->d_name, ".manifest")) continue;
                char fpath[2048];
                snprintf(fpath, sizeof(fpath), "%s/%s", snapdir, fe->d_name);
                struct stat fst;
                if (lstat(fpath, &fst) == 0 && S_ISREG(fst.st_mode)) files++;
            }
            closedir(sd);

            char mpath[1100];
            snprintf(mpath, sizeof(mpath), "%s/.manifest", snapdir);
            FILE *mf = fopen(mpath, "r");
            if (mf) {
                char ln[512];
                while (fgets(ln, sizeof(ln), mf)) manifest_lines++;
                fclose(mf);
            }

            if (files == 0 && manifest_lines == 0) {
                /* Safe to nuke — unlink the maybe-empty manifest and
                   rmdir the snap dir. */
                unlink(mpath);
                if (rmdir(snapdir) == 0) {
                    pruned++;
                    fprintf(stderr,
                            "backup_prune: removed empty %s\n", snapdir);
                }
            }
        }
        closedir(td);

        rmdir(tagdir);
    }
    closedir(root);
    return pruned;
}


static int
cap_snapshots_per_tag(int keep) {
    DIR *root = opendir(BACKUPS_ROOT);
    if (!root) return 0;
    int removed = 0;
    struct dirent *te;
    while ((te = readdir(root))) {
        if (te->d_name[0] == '.') continue;
        if (!strcmp(te->d_name, "initial")) continue;  /* protected */

        char tagdir[768];
        snprintf(tagdir, sizeof(tagdir), "%s/%s", BACKUPS_ROOT, te->d_name);
        struct stat st;
        if (stat(tagdir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        /* Collect ts entries, sort desc (newest first). */
        char (*entries)[64] = NULL;
        int n = 0, cap = 0;
        DIR *td = opendir(tagdir);
        if (!td) continue;
        struct dirent *se;
        while ((se = readdir(td))) {
            if (se->d_name[0] == '.') continue;
            if (n == cap) {
                int new_cap = cap ? cap * 2 : 16;
                char (*nb)[64] = realloc(entries, (size_t)new_cap * 64);
                if (!nb) break;
                entries = nb;
                cap = new_cap;
            }
            strncpy(entries[n], se->d_name, 63);
            entries[n][63] = 0;
            n++;
        }
        closedir(td);

        /* Bubble sort descending (n is tiny, ~tens at worst). */
        for (int i = 0; i < n; i++) {
            for (int j = i + 1; j < n; j++) {
                long long a = atoll(entries[i]);
                long long b = atoll(entries[j]);
                if (b > a) {
                    char tmp[64];
                    memcpy(tmp, entries[i], 64);
                    memcpy(entries[i], entries[j], 64);
                    memcpy(entries[j], tmp, 64);
                }
            }
        }

        /* Anything past the keep window → rm -rf the snapshot dir. */
        for (int i = keep; i < n; i++) {
            char snapdir[1024];
            snprintf(snapdir, sizeof(snapdir), "%s/%s",
                     tagdir, entries[i]);
            DIR *sd = opendir(snapdir);
            if (sd) {
                struct dirent *fe;
                while ((fe = readdir(sd))) {
                    if (!strcmp(fe->d_name, ".") ||
                        !strcmp(fe->d_name, "..")) continue;
                    char fpath[2048];
                    snprintf(fpath, sizeof(fpath), "%s/%s",
                             snapdir, fe->d_name);
                    unlink(fpath);
                }
                closedir(sd);
            }
            if (rmdir(snapdir) == 0) {
                removed++;
                fprintf(stderr,
                        "backup_cap: removed %s (over keep=%d)\n",
                        snapdir, keep);
            }
        }
        free(entries);
    }
    closedir(root);
    return removed;
}


void
backup_init(void) {
    mkpath_p(BACKUPS_ROOT);

    /* Always run prune first — cheap, even on first boot it's a no-op
       since there are no leftover dirs to find. */
    int pruned = prune_empty_snapshots();
    if (pruned > 0) {
        fprintf(stderr,
                "backup_init: pruned %d empty snapshot dir(s)\n", pruned);
    }

    /* Then cap per-tag history. Keeps the UI tidy when boot-time
       snapshots accumulate across many reboots. */
    int capped = cap_snapshots_per_tag(BACKUPS_KEEP_PER_TAG);
    if (capped > 0) {
        fprintf(stderr,
                "backup_init: capped %d snapshot(s) past keep=%d\n",
                capped, BACKUPS_KEEP_PER_TAG);
    }

    struct stat st;
    if (stat(BACKUPS_INIT_FLAG, &st) == 0) return;        /* already done */

    fprintf(stderr, "backup_init: creating pristine 'initial' snapshot\n");
    static const char *initial_targets[] = {
        "/system_data/priv/mms/app.db",
        "/system_data/priv/mms/appinfo.db",
        "/system_data/priv/mms/addcont.db",
        "/system_data/priv/mms/notification2.db",
        NULL,
    };
    for (int i = 0; initial_targets[i]; i++) {
        backup_snapshot_file("initial", initial_targets[i]);
    }

    int fd = open(BACKUPS_INIT_FLAG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        const char *m = "elf-arsenal: backups initialized\n";
        ssize_t w = write(fd, m, strlen(m));
        (void)w;
        close(fd);
    }
}


/* ── dedicated backup-worker thread ───────────────────────────────── */

typedef enum {
    BJ_DUMP_REGISTRY = 1,
    BJ_RESTORE,
    BJ_DELETE,
} bj_kind_t;

typedef struct {
    bj_kind_t kind;
    char      tag[64];
    char      ts[64];
} bj_job_t;

#define BJ_QUEUE_CAP 8
static bj_job_t        g_bj_queue[BJ_QUEUE_CAP];
static int             g_bj_head    = 0;
static int             g_bj_tail    = 0;
static int             g_bj_count   = 0;
static int             g_bj_running = 0;
static pthread_mutex_t g_bj_lock    = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_bj_cond    = PTHREAD_COND_INITIALIZER;
static int             g_bj_worker_up = 0;

/* Last completed job status (UI-pollable via /api/backup/job-status). */
static char            g_bj_last_status[1024] =
    "{\"ok\":true,\"idle\":true}";
static pthread_mutex_t g_bj_status_lock = PTHREAD_MUTEX_INITIALIZER;

static void
bj_set_status(const char *json) {
    pthread_mutex_lock(&g_bj_status_lock);
    strncpy(g_bj_last_status, json, sizeof(g_bj_last_status) - 1);
    g_bj_last_status[sizeof(g_bj_last_status) - 1] = 0;
    pthread_mutex_unlock(&g_bj_status_lock);
}

/* bj_get_status removed — /api/backup/job-status now reads the helper's
   on-disk status file directly instead of the in-process worker buffer. */

/* Forward decl — restore_from_manifest is below. */
static int restore_from_manifest(const char *snap_dir);
static int rm_rf(const char *path);

static void
bj_execute(const bj_job_t *job) {
    char status_buf[512];
    switch (job->kind) {
        case BJ_DUMP_REGISTRY: {
            dump_registry_work_inner();
            snprintf(status_buf, sizeof(status_buf),
                "{\"ok\":true,\"finished\":true,\"kind\":\"dump-registry\","
                "\"finishedAt\":%lld}",
                (long long)time(NULL));
            break;
        }
        case BJ_RESTORE: {
            char snapdir[768];
            snprintf(snapdir, sizeof(snapdir), "%s/%s/%s",
                     BACKUPS_ROOT, job->tag, job->ts);
            int n = restore_from_manifest(snapdir);
            snprintf(status_buf, sizeof(status_buf),
                "{\"ok\":%s,\"finished\":true,\"kind\":\"restore\","
                "\"tag\":\"%s\",\"ts\":\"%s\",\"restored\":%d,"
                "\"finishedAt\":%lld}",
                n >= 0 ? "true" : "false",
                job->tag, job->ts, n,
                (long long)time(NULL));
            break;
        }
        case BJ_DELETE: {
            char snapdir[768];
            snprintf(snapdir, sizeof(snapdir), "%s/%s/%s",
                     BACKUPS_ROOT, job->tag, job->ts);
            int rc = rm_rf(snapdir);
            char tagdir[640];
            snprintf(tagdir, sizeof(tagdir), "%s/%s",
                     BACKUPS_ROOT, job->tag);
            rmdir(tagdir);    /* harmless if non-empty */
            snprintf(status_buf, sizeof(status_buf),
                "{\"ok\":%s,\"finished\":true,\"kind\":\"delete\","
                "\"tag\":\"%s\",\"ts\":\"%s\","
                "\"finishedAt\":%lld}",
                rc == 0 ? "true" : "false",
                job->tag, job->ts,
                (long long)time(NULL));
            break;
        }
        default:
            snprintf(status_buf, sizeof(status_buf),
                "{\"ok\":false,\"finished\":true,\"error\":\"unknown job\"}");
    }
    bj_set_status(status_buf);
}

static void *
backup_worker_loop(void *unused) {
    (void)unused;
    fprintf(stderr,
        "backup_worker_loop: started (pid=%d tid=%lu) — ready to drain jobs\n",
        getpid(), (unsigned long)pthread_self());
    while (1) {
        bj_job_t job;
        pthread_mutex_lock(&g_bj_lock);
        while (g_bj_count == 0) {
            pthread_cond_wait(&g_bj_cond, &g_bj_lock);
        }
        job = g_bj_queue[g_bj_head];
        g_bj_head = (g_bj_head + 1) % BJ_QUEUE_CAP;
        g_bj_count--;
        g_bj_running = 1;
        pthread_mutex_unlock(&g_bj_lock);

        char run_buf[256];
        snprintf(run_buf, sizeof(run_buf),
            "{\"ok\":true,\"running\":true,\"kind\":%d,"
            "\"tag\":\"%s\",\"ts\":\"%s\","
            "\"startedAt\":%lld}",
            (int)job.kind, job.tag, job.ts, (long long)time(NULL));
        bj_set_status(run_buf);

        bj_execute(&job);

        pthread_mutex_lock(&g_bj_lock);
        g_bj_running = 0;
        pthread_mutex_unlock(&g_bj_lock);
    }
    return NULL;
}

int
backup_worker_init(void) {
    if (g_bj_worker_up) return 0;
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &a, backup_worker_loop, NULL);
    pthread_attr_destroy(&a);
    if (rc != 0) {
        fprintf(stderr,
            "backup_worker_init: pthread_create failed: %s\n",
            strerror(errno));
        return -1;
    }
    g_bj_worker_up = 1;
    return 0;
}

static int
bj_enqueue(bj_kind_t kind, const char *tag, const char *ts) {
    if (!g_bj_worker_up) return -2;
    pthread_mutex_lock(&g_bj_lock);
    if (g_bj_count >= BJ_QUEUE_CAP) {
        pthread_mutex_unlock(&g_bj_lock);
        return -1;
    }
    bj_job_t *slot = &g_bj_queue[g_bj_tail];
    slot->kind = kind;
    if (tag) { strncpy(slot->tag, tag, sizeof(slot->tag) - 1);
               slot->tag[sizeof(slot->tag) - 1] = 0; }
    else slot->tag[0] = 0;
    if (ts)  { strncpy(slot->ts, ts, sizeof(slot->ts) - 1);
               slot->ts[sizeof(slot->ts) - 1] = 0; }
    else slot->ts[0] = 0;
    g_bj_tail = (g_bj_tail + 1) % BJ_QUEUE_CAP;
    g_bj_count++;
    pthread_cond_signal(&g_bj_cond);
    pthread_mutex_unlock(&g_bj_lock);
    return 0;
}

/* Called from backup_dump_registry() (which routes through here now). */
static int
bj_enqueue_dump_registry(void) {
    return bj_enqueue(BJ_DUMP_REGISTRY, NULL, NULL);
}


/* ── restore ─────────────────────────────────────────────────────── */

static int
restore_from_manifest(const char *snap_dir) {
    char mpath[1024];
    snprintf(mpath, sizeof(mpath), "%s/.manifest", snap_dir);
    FILE *f = fopen(mpath, "r");
    if (!f) return -1;
    int n = 0;
    char line[1100];
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline. */
        size_t L = strlen(line);
        while (L > 0 && (line[L-1] == '\n' || line[L-1] == '\r'))
            line[--L] = 0;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        const char *snap_basename = line;
        const char *original = tab + 1;
        char src[1024];
        snprintf(src, sizeof(src), "%s/%s", snap_dir, snap_basename);
        if (copy_file(src, original) == 0) {
            n++;
            fprintf(stderr, "restore: %s -> %s\n", src, original);
        }
    }
    fclose(f);
    return n;
}


/* ── HTTP API ────────────────────────────────────────────────────── */

static enum MHD_Result
serve_json_owned(struct MHD_Connection *conn, unsigned status, cJSON *r) {
    char *txt = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (!txt) return MHD_NO;
    size_t len = strlen(txt);
    struct MHD_Response *resp =
        MHD_create_response_from_buffer(len, txt, MHD_RESPMEM_MUST_FREE);
    if (!resp) { free(txt); return MHD_NO; }
    MHD_add_response_header(resp, "Content-Type", "application/json");
    MHD_add_response_header(resp, "Cache-Control", "no-cache");
    enum MHD_Result rc = websrv_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    return rc;
}


static cJSON*
list_snapshots(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON *tags = cJSON_AddArrayToObject(root, "tags");
    DIR *d = opendir(BACKUPS_ROOT);
    if (!d) {
        cJSON_AddBoolToObject(root, "ok", 1);
        return root;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (e->d_name[0] == '.') continue;
        char tagdir[512];
        snprintf(tagdir, sizeof(tagdir), "%s/%s", BACKUPS_ROOT, e->d_name);
        struct stat st;
        if (stat(tagdir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        cJSON *tag = cJSON_CreateObject();
        cJSON_AddStringToObject(tag, "name", e->d_name);
        const tag_meta_t *meta = tag_meta_lookup(e->d_name);
        cJSON_AddStringToObject(tag, "label",
                                meta ? meta->label : e->d_name);
        cJSON_AddStringToObject(tag, "description",
                                meta ? meta->description :
                                "Snapshot taken by a feature this build "
                                "doesn't have built-in metadata for. "
                                "Restore at your own risk.");
        cJSON_AddBoolToObject(tag,   "protected",
                              !strcmp(e->d_name, "initial"));
        cJSON_AddNumberToObject(tag, "keepLimit", BACKUPS_KEEP_PER_TAG);
        cJSON *snaps = cJSON_AddArrayToObject(tag, "snapshots");

        DIR *td = opendir(tagdir);
        if (td) {
            struct dirent *te;
            while ((te = readdir(td))) {
                if (te->d_name[0] == '.') continue;
                char snapdir[640];
                snprintf(snapdir, sizeof(snapdir), "%s/%s", tagdir, te->d_name);
                if (stat(snapdir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
                /* Count files in manifest. */
                int file_count = 0;
                char mpath[768];
                snprintf(mpath, sizeof(mpath), "%s/.manifest", snapdir);
                FILE *mf = fopen(mpath, "r");
                if (mf) {
                    char ln[512];
                    while (fgets(ln, sizeof(ln), mf)) file_count++;
                    fclose(mf);
                }
                cJSON *snap = cJSON_CreateObject();
                cJSON_AddStringToObject(snap, "ts", te->d_name);
                cJSON_AddNumberToObject(snap, "files", file_count);
                cJSON_AddItemToArray(snaps, snap);
            }
            closedir(td);
        }
        cJSON_AddItemToArray(tags, tag);
    }
    closedir(d);
    cJSON_AddBoolToObject(root, "ok", 1);
    return root;
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
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
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



static atomic_int g_helper_busy = 0;

static int
bh_status_finished(void) {
    int fd = open(BACKUP_JOB_STATUS_FILE, O_RDONLY);
    if (fd < 0) return 0;
    char buf[2048];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    return strstr(buf, "\"finished\":true") != NULL;
}

static int
bh_is_busy(void) {
    if (!atomic_load(&g_helper_busy)) return 0;
    if (bh_status_finished()) {
        atomic_store(&g_helper_busy, 0);
        return 0;
    }
    return 1;
}

static int
bh_spawn_serialized(const char *op, const char *tag, const char *ts) {
    if (bh_is_busy()) return -1;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_helper_busy, &expected, 1)) {
        return -1;
    }

    int fd = open(BACKUP_JOB_STATUS_FILE,
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        const char *pending = "{\"ok\":true,\"pending\":true}";
        (void)write(fd, pending, strlen(pending));
        close(fd);
    }

    int rc;
    if (tag && ts) {
        const char *argv[3] = { op, tag, ts };
        rc = sys_spawn_backup_helper(3, argv);
    } else {
        const char *argv[1] = { op };
        rc = sys_spawn_backup_helper(1, argv);
    }
    if (rc != 0) {
        atomic_store(&g_helper_busy, 0);
        return -2;
    }
    return 0;
}



static int
status_finished_yet(void) {
    int fd = open(BACKUP_JOB_STATUS_FILE, O_RDONLY);
    if (fd < 0) return 0;
    char buf[2048];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    return strstr(buf, "\"finished\":true") != NULL;
}

void *
restore_orchestrator_main(void *arg) {
    char *job = arg;
    char *sep = strchr(job, '\t');
    if (!sep) { free(job); return NULL; }
    *sep = 0;
    const char *tag = job;
    const char *ts  = sep + 1;

    /* Let the HTTP response flush to the browser before we yank the
       carpet out from under websrv. */
    usleep(800 * 1000);

    /* Pre-clear the helper's status file so the UI can distinguish
       "this restore's status" from "stale prior status". */
    int fd = open(BACKUP_JOB_STATUS_FILE,
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        const char *pending =
            "{\"ok\":true,\"pending\":true,\"kind\":\"restore\"}";
        (void)write(fd, pending, strlen(pending));
        close(fd);
    }

    fprintf(stderr,
        "restore_orchestrator: pausing websrv for restore %s/%s\n",
        tag, ts);
    websrv_pause();
    /* websrv_listen's accept() loop will exit; main()'s outer loop
       sleeps 3 s before restarting it, giving us a clean window. */

    const char *argv[3] = { "restore", tag, ts };
    int rc = sys_spawn_backup_helper(3, argv);
    if (rc != 0) {
        fprintf(stderr,
            "restore_orchestrator: helper spawn failed rc=%d\n", rc);
        free(job);
        return NULL;
    }

    int waited = 0;
    while (waited < 90 && !status_finished_yet()) {
        usleep(500 * 1000);
        waited++;
    }
    fprintf(stderr,
        "restore_orchestrator: helper finished after ~%d 500ms ticks; "
        "websrv will respawn within ~3s via main loop\n", waited);
    atomic_store(&g_helper_busy, 0);
    free(job);
    return NULL;
}


/* Build the {ok, queued/spawned, kind, ...} response for a helper spawn. */
static cJSON *
helper_response(int rc, const char *kind, const char *tag, const char *ts) {
    cJSON *r = cJSON_CreateObject();
    if (rc == 0) {
        cJSON_AddBoolToObject  (r, "ok",       1);
        cJSON_AddBoolToObject  (r, "queued",   1);
        cJSON_AddStringToObject(r, "kind",     kind);
        if (tag) cJSON_AddStringToObject(r, "tag", tag);
        if (ts)  cJSON_AddStringToObject(r, "ts",  ts);
        cJSON_AddStringToObject(r, "statusUrl", "/api/backup/job-status");
        cJSON_AddStringToObject(r, "note",
            "backup-helper.elf spawned as a separate process. Poll "
            "/api/backup/job-status to watch it complete.");
    } else {
        cJSON_AddBoolToObject  (r, "ok",       0);
        cJSON_AddStringToObject(r, "error",
            "Failed to spawn backup-helper.elf — check the system log.");
    }
    return r;
}

enum MHD_Result
backup_request(struct MHD_Connection *conn, const char *url) {
    if (!strcmp(url, "/api/backup/list") || !strcmp(url, "/api/backup")) {
        return serve_json_owned(conn, MHD_HTTP_OK, list_snapshots());
    }
    if (!strcmp(url, "/api/backup/job-status")) {
        (void)bh_is_busy();
        /* Read the helper's status file. If it doesn't exist yet,
           report idle. */
        int fd = open(BACKUP_JOB_STATUS_FILE, O_RDONLY);
        if (fd < 0) {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddBoolToObject(r, "ok",   1);
            cJSON_AddBoolToObject(r, "idle", 1);
            return serve_json_owned(conn, MHD_HTTP_OK, r);
        }
        char buf[2048];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddBoolToObject(r, "ok",   1);
            cJSON_AddBoolToObject(r, "idle", 1);
            return serve_json_owned(conn, MHD_HTTP_OK, r);
        }
        buf[n] = 0;
        struct MHD_Response *resp =
            MHD_create_response_from_buffer((size_t)n, buf, MHD_RESPMEM_MUST_COPY);
        if (!resp) return MHD_NO;
        MHD_add_response_header(resp, "Content-Type",  "application/json");
        MHD_add_response_header(resp, "Cache-Control", "no-cache");
        enum MHD_Result rc = websrv_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return rc;
    }
    if (!strcmp(url, "/api/backup/profile") ||
        !strcmp(url, "/api/backup/trophies")) {
        const char *op = strrchr(url, '/') + 1;
        uint32_t uid = sys_get_foreground_user(NULL, 0);
        if (!uid) {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddBoolToObject(r, "ok", 0);
            cJSON_AddStringToObject(r, "error",
                "no foreground user — sign in to a profile first");
            return serve_json_owned(conn, MHD_HTTP_BAD_REQUEST, r);
        }
        char uid_hex[16];
        snprintf(uid_hex, sizeof(uid_hex), "%x", uid);
        if (bh_is_busy()) {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddBoolToObject(r, "ok", 0);
            cJSON_AddBoolToObject(r, "busy", 1);
            cJSON_AddStringToObject(r, "error",
                "Another backup helper is already running. Try again "
                "in a few seconds.");
            return serve_json_owned(conn, MHD_HTTP_SERVICE_UNAVAILABLE, r);
        }
        int expected = 0;
        if (!atomic_compare_exchange_strong(&g_helper_busy, &expected, 1)) {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddBoolToObject(r, "ok", 0);
            cJSON_AddBoolToObject(r, "busy", 1);
            cJSON_AddStringToObject(r, "error", "busy");
            return serve_json_owned(conn, MHD_HTTP_SERVICE_UNAVAILABLE, r);
        }
        /* Pre-clear status file. */
        int fd = open(BACKUP_JOB_STATUS_FILE,
                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            const char *pending = "{\"ok\":true,\"pending\":true}";
            (void)write(fd, pending, strlen(pending));
            close(fd);
        }
        const char *argv[2] = { op, uid_hex };
        int rc = sys_spawn_backup_helper(2, argv);
        if (rc != 0) {
            atomic_store(&g_helper_busy, 0);
        }
        return serve_json_owned(conn, rc == 0 ? MHD_HTTP_OK
                                              : MHD_HTTP_INTERNAL_SERVER_ERROR,
                                helper_response(rc == 0 ? 0 : -1, op, NULL, NULL));
    }
    if (!strcmp(url, "/api/backup/dump-registry")) {
        int rc = bh_spawn_serialized("dump-registry", NULL, NULL);
        if (rc == -1) {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddBoolToObject(r, "ok", 0);
            cJSON_AddBoolToObject(r, "busy", 1);
            cJSON_AddStringToObject(r, "error",
                "Another backup helper is already running. Try again "
                "in a few seconds.");
            return serve_json_owned(conn, MHD_HTTP_SERVICE_UNAVAILABLE, r);
        }
        return serve_json_owned(conn, rc == 0 ? MHD_HTTP_OK
                                              : MHD_HTTP_INTERNAL_SERVER_ERROR,
                                helper_response(rc == 0 ? 0 : -1,
                                                "dump-registry", NULL, NULL));
    }
    if (!strcmp(url, "/api/backup/restore")) {
        const char *tag = MHD_lookup_connection_value(conn,
            MHD_GET_ARGUMENT_KIND, "tag");
        const char *ts  = MHD_lookup_connection_value(conn,
            MHD_GET_ARGUMENT_KIND, "ts");
        if (!tag || !ts || strchr(tag, '/') || strchr(ts, '/')) {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddBoolToObject(r, "ok", 0);
            cJSON_AddStringToObject(r, "error", "missing/invalid tag or ts");
            return serve_json_owned(conn, MHD_HTTP_BAD_REQUEST, r);
        }
        char snapdir[768];
        snprintf(snapdir, sizeof(snapdir), "%s/%s/%s",
                 BACKUPS_ROOT, tag, ts);
        struct stat st;
        if (stat(snapdir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddBoolToObject(r, "ok", 0);
            cJSON_AddStringToObject(r, "error", "snapshot not found");
            return serve_json_owned(conn, MHD_HTTP_NOT_FOUND, r);
        }
        {
            int expected = 0;
            if (!atomic_compare_exchange_strong(&g_helper_busy,
                                                 &expected, 1)) {
                cJSON *r = cJSON_CreateObject();
                cJSON_AddBoolToObject(r, "ok", 0);
                cJSON_AddBoolToObject(r, "busy", 1);
                cJSON_AddStringToObject(r, "error",
                    "Another backup helper is already running. Try "
                    "again in a few seconds.");
                return serve_json_owned(conn,
                    MHD_HTTP_SERVICE_UNAVAILABLE, r);
            }
        }
        char *job_arg = malloc(192);
        if (!job_arg) {
            atomic_store(&g_helper_busy, 0);
            cJSON *r = cJSON_CreateObject();
            cJSON_AddBoolToObject(r, "ok", 0);
            cJSON_AddStringToObject(r, "error", "OOM");
            return serve_json_owned(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, r);
        }
        snprintf(job_arg, 192, "%s\t%s", tag, ts);
        pthread_t orch_t;
        pthread_attr_t orch_a;
        pthread_attr_init(&orch_a);
        pthread_attr_setdetachstate(&orch_a, PTHREAD_CREATE_DETACHED);
        extern void *restore_orchestrator_main(void *arg);
        int orc = pthread_create(&orch_t, &orch_a,
                                 restore_orchestrator_main, job_arg);
        pthread_attr_destroy(&orch_a);
        if (orc != 0) {
            atomic_store(&g_helper_busy, 0);
            free(job_arg);
            cJSON *r = cJSON_CreateObject();
            cJSON_AddBoolToObject(r, "ok", 0);
            cJSON_AddStringToObject(r, "error",
                "Could not spawn restore orchestrator");
            return serve_json_owned(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, r);
        }
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject  (r, "ok",                1);
        cJSON_AddBoolToObject  (r, "restoring",         1);
        cJSON_AddBoolToObject  (r, "restartingWebsrv",  1);
        cJSON_AddStringToObject(r, "tag",               tag);
        cJSON_AddStringToObject(r, "ts",                ts);
        cJSON_AddStringToObject(r, "statusUrl",
                                "/api/backup/job-status");
        cJSON_AddStringToObject(r, "note",
            "Restore queued. Web server is about to pause for ~5-30 s "
            "while a helper process writes /system_data files. The UI "
            "should auto-reconnect once it's back; if not, hard-refresh "
            "the page in a few seconds.");
        return serve_json_owned(conn, MHD_HTTP_OK, r);
    }
    if (!strcmp(url, "/api/backup/delete")) {
        const char *tag = MHD_lookup_connection_value(conn,
            MHD_GET_ARGUMENT_KIND, "tag");
        const char *ts  = MHD_lookup_connection_value(conn,
            MHD_GET_ARGUMENT_KIND, "ts");
        if (!tag || !ts || strchr(tag, '/') || strchr(ts, '/')) {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddBoolToObject(r, "ok", 0);
            cJSON_AddStringToObject(r, "error", "missing/invalid tag or ts");
            return serve_json_owned(conn, MHD_HTTP_BAD_REQUEST, r);
        }
        int rc = bh_spawn_serialized("delete", tag, ts);
        if (rc == -1) {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddBoolToObject(r, "ok", 0);
            cJSON_AddBoolToObject(r, "busy", 1);
            cJSON_AddStringToObject(r, "error",
                "Another backup helper is already running. Try again "
                "in a few seconds.");
            return serve_json_owned(conn, MHD_HTTP_SERVICE_UNAVAILABLE, r);
        }
        return serve_json_owned(conn, rc == 0 ? MHD_HTTP_OK
                                              : MHD_HTTP_INTERNAL_SERVER_ERROR,
                                helper_response(rc == 0 ? 0 : -1,
                                                "delete", tag, ts));
    }
    cJSON *err = cJSON_CreateObject();
    cJSON_AddBoolToObject(err, "ok", 0);
    cJSON_AddStringToObject(err, "error", "no such endpoint");
    return serve_json_owned(conn, MHD_HTTP_NOT_FOUND, err);
}
