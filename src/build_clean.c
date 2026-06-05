/* Elf Arsenal — per-build data directory cleanup.
 *
 * On the first run of each new build, wipes /data/elf-arsenal of any
 * stale files left by a previous build, keeping only:
 *   config.ini                — user settings
 *   cheats/                   — downloaded cheat files
 *   .build_stamp              — this stamp file (read first, written last)
 *   .elf_arsenal_migrate_v1   — sonic-loader migration marker
 *
 * The stamp stores EA_VERSION (git-describe string baked at compile time).
 * A mismatch means a new ELF was deployed; a match means already cleaned.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "version.h"
#include "build_clean.h"

#define ARSENAL_DIR  "/data/elf-arsenal"
#define STAMP_PATH   "/data/elf-arsenal/.build_stamp"

static const char *KEEP[] = {
    "config.ini",
    "cheats",
    ".build_stamp",
    ".elf_arsenal_migrate_v1",
    NULL,
};

static int
is_keep(const char *name) {
    for (int i = 0; KEEP[i]; i++)
        if (!strcmp(name, KEEP[i])) return 1;
    return 0;
}

static void
rm_recursive(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;

    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char child[1024];
                snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
                rm_recursive(child);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

static void
read_stamp(char *buf, size_t sz) {
    buf[0] = '\0';
    int fd = open(STAMP_PATH, O_RDONLY);
    if (fd < 0) return;
    ssize_t n = read(fd, buf, (ssize_t)sz - 1);
    close(fd);
    if (n > 0) {
        buf[n] = '\0';
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';
    }
}

static void
write_stamp(void) {
    mkdir(ARSENAL_DIR, 0755);
    int fd = open(STAMP_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    write(fd, EA_VERSION, strlen(EA_VERSION));
    write(fd, "\n", 1);
    fsync(fd);
    close(fd);
}

void
build_clean_run_once(void) {
    char stored[256];
    read_stamp(stored, sizeof(stored));

    if (stored[0] && !strcmp(stored, EA_VERSION)) return;

    fprintf(stderr,
            "build_clean: new build \"%s\" (stamp was \"%s\") "
            "— cleaning %s\n",
            EA_VERSION, stored[0] ? stored : "(none)", ARSENAL_DIR);

    DIR *d = opendir(ARSENAL_DIR);
    if (!d) {
        write_stamp();
        return;
    }

    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (is_keep(e->d_name)) continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", ARSENAL_DIR, e->d_name);
        rm_recursive(path);
        fprintf(stderr, "build_clean: removed %s\n", path);
    }
    closedir(d);

    write_stamp();
    fprintf(stderr, "build_clean: done — stamp = %s\n", EA_VERSION);
}
