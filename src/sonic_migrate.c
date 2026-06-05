/* Elf Arsenal — one-shot migration from sonic-loader.elf to
   elf-arsenal.elf. See sonic_migrate.h for the rationale. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sonic_migrate.h"


#define MIGRATE_STATE_DIR   "/data/elf-arsenal"
#define MIGRATE_STATE_PATH  "/data/elf-arsenal/.elf_arsenal_migrate_v1"

/* Roots we scan for the old payload. Anything not present at runtime
   gets silently skipped. */
static const char *SCAN_ROOTS[] = {
    "/mnt/usb0",
    "/mnt/usb1",
    "/mnt/usb2",
    "/mnt/usb3",
    "/mnt/usb4",
    "/mnt/usb5",
    "/mnt/usb6",
    "/mnt/usb7",
    "/mnt/ext0",
    "/mnt/ext1",
    "/data",
    "/user/data",
    NULL,
};

#define MAX_SCAN_DEPTH  6

/* Hard caps on per-scan work to keep the boot path snappy. */
#define MAX_FILES_TOTAL 200000
#define MAX_RENAMES     32


static int g_files_seen   = 0;
static int g_renames_done = 0;
/* Log buffer we serialize into the marker file. ~120 chars per entry,
   bounded by 8 KB so even hostile filesystems can't blow it up. */
#define MIGRATE_LOG_MAX  8192
static char g_log[MIGRATE_LOG_MAX];
static size_t g_log_len = 0;

static void
log_line(const char *fmt, ...) {
    if(g_log_len >= MIGRATE_LOG_MAX - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_log + g_log_len, MIGRATE_LOG_MAX - g_log_len, fmt, ap);
    va_end(ap);
    if(n > 0) g_log_len += (size_t)n;
}


static int
is_elf_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if(fd < 0) return 0;
    unsigned char buf[4];
    ssize_t n = read(fd, buf, 4);
    close(fd);
    if(n != 4) return 0;
    return (buf[0] == 0x7f && buf[1] == 'E' &&
            buf[2] == 'L'  && buf[3] == 'F');
}


static int
do_rename_sl_to_ea(const char *dir, const char *old_name) {
    char old_path[1024];
    char new_path[1024];
    snprintf(old_path, sizeof(old_path), "%s/%s", dir, old_name);
    snprintf(new_path, sizeof(new_path), "%s/%s", dir, "elf-arsenal.elf");

    /* If a fresh elf-arsenal.elf is already sitting next to the old
       one, the new ELF takes precedence — just delete the legacy. */
    struct stat st;
    if(stat(new_path, &st) == 0) {
        if(unlink(old_path) == 0) {
            log_line("delete legacy (new already present): %s\n", old_path);
            return 1;
        }
        return 0;
    }

    if(rename(old_path, new_path) == 0) {
        log_line("rename: %s -> elf-arsenal.elf\n", old_path);
        return 1;
    }
    return 0;
}


/* Recursive worker. Walks one directory tree, performing the rename pass. */
static void
scan_dir(const char *dir, int depth) {
    if(depth > MAX_SCAN_DEPTH) return;
    if(g_files_seen   >= MAX_FILES_TOTAL) return;
    if(g_renames_done >= MAX_RENAMES) return;

    DIR *d = opendir(dir);
    if(!d) return;

    struct dirent *e;
    while((e = readdir(d))) {
        if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        g_files_seen++;
        if(g_files_seen > MAX_FILES_TOTAL) break;

        char path[1024];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if(n <= 0 || (size_t)n >= sizeof(path)) continue;

        struct stat st;
        if(lstat(path, &st) != 0) continue;
        if(S_ISLNK(st.st_mode)) continue;   /* don't follow symlinks */

        if(S_ISDIR(st.st_mode)) {
            scan_dir(path, depth + 1);
            continue;
        }
        if(!S_ISREG(st.st_mode)) continue;

        if(!strcmp(e->d_name, "sonic-loader.elf")) {
            if(g_renames_done < MAX_RENAMES && is_elf_file(path)) {
                if(do_rename_sl_to_ea(dir, e->d_name))
                    g_renames_done++;
            }
        }
    }
    closedir(d);
}


static void
write_marker(void) {
    mkdir(MIGRATE_STATE_DIR, 0755);
    int fd = open(MIGRATE_STATE_PATH,
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd < 0) return;
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "elf-arsenal migration v1\n"
                     "files_seen=%d  renames=%d\n"
                     "--- log ---\n",
                     g_files_seen, g_renames_done);
    if(n > 0) write(fd, hdr, (size_t)n);
    if(g_log_len) write(fd, g_log, g_log_len);
    fsync(fd);
    close(fd);
}


void
sonic_migrate_run_once(void) {
    /* Cheap idempotency check: the marker file's presence == "we
       already ran on this console, do nothing". */
    struct stat st;
    if(stat(MIGRATE_STATE_PATH, &st) == 0) return;

    fprintf(stderr,
            "sonic_migrate: scanning disks for sonic-loader.elf "
            "(one-shot upgrade pass)\n");

    /* The actual scan. Each root may or may not exist at runtime —
       opendir() just returns NULL on missing mounts, which is fine. */
    for(int i = 0; SCAN_ROOTS[i]; i++) {
        scan_dir(SCAN_ROOTS[i], 0);
    }

    fprintf(stderr,
            "sonic_migrate: scan complete — files_seen=%d  renames=%d\n",
            g_files_seen, g_renames_done);

    write_marker();
}



#define DATA_OLD_DIR  "/data/sonic-loader"
#define DATA_NEW_DIR  "/data/elf-arsenal"


static int
recursive_move(const char *src, const char *dst) {
    /* Make sure dst exists. */
    struct stat st;
    if(stat(dst, &st) != 0) {
        if(mkdir(dst, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr,
                "data_dir_migrate: mkdir %s failed: %s\n",
                dst, strerror(errno));
            return -1;
        }
    }

    DIR *d = opendir(src);
    if(!d) {
        fprintf(stderr,
            "data_dir_migrate: opendir %s failed: %s\n",
            src, strerror(errno));
        return -1;
    }

    struct dirent *e;
    int moved = 0, skipped = 0;
    while((e = readdir(d))) {
        if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;

        char from[1024], to[1024];
        if(snprintf(from, sizeof(from), "%s/%s", src, e->d_name) >= (int)sizeof(from)) continue;
        if(snprintf(to,   sizeof(to),   "%s/%s", dst, e->d_name) >= (int)sizeof(to))   continue;

        if(lstat(from, &st) != 0) continue;

        if(S_ISDIR(st.st_mode)) {
            recursive_move(from, to);
            /* Try to drop the now-empty source dir. */
            rmdir(from);
        } else {
            struct stat dst_st;
            if(stat(to, &dst_st) == 0) {
                skipped++;
                continue;
            }
            if(rename(from, to) == 0) {
                moved++;
            } else {
                fprintf(stderr,
                    "data_dir_migrate: rename %s -> %s failed: %s\n",
                    from, to, strerror(errno));
            }
        }
    }
    closedir(d);
    fprintf(stderr,
        "data_dir_migrate: %s -> %s  moved=%d  skipped=%d\n",
        src, dst, moved, skipped);
    return 0;
}


void
data_dir_migrate(void) {
    struct stat st_old, st_new;
    int old_exists = (stat(DATA_OLD_DIR, &st_old) == 0 &&
                      S_ISDIR(st_old.st_mode));
    int new_exists = (stat(DATA_NEW_DIR, &st_new) == 0 &&
                      S_ISDIR(st_new.st_mode));

    if(!old_exists && !new_exists) {
        mkdir(DATA_NEW_DIR, 0755);
        return;
    }

    /* ── case 3: already migrated. Nothing to do. ── */
    if(!old_exists && new_exists) return;

    if(old_exists && !new_exists) {
        if(rename(DATA_OLD_DIR, DATA_NEW_DIR) == 0) {
            fprintf(stderr,
                "data_dir_migrate: renamed %s -> %s "
                "(all user data preserved)\n",
                DATA_OLD_DIR, DATA_NEW_DIR);
            return;
        }
        fprintf(stderr,
            "data_dir_migrate: atomic rename failed (%s), "
            "falling back to recursive move\n",
            strerror(errno));
    }

    /* ── case 2 (or rename fallback): both exist — merge old into
       new with dest-wins conflict policy. ── */
    fprintf(stderr,
        "data_dir_migrate: merging %s into %s "
        "(dest-wins on conflict)\n",
        DATA_OLD_DIR, DATA_NEW_DIR);
    recursive_move(DATA_OLD_DIR, DATA_NEW_DIR);
    rmdir(DATA_OLD_DIR);
}
