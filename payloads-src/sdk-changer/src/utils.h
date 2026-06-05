
#ifndef SDK_CHANGER_UTILS_H
#define SDK_CHANGER_UTILS_H

#include <stdio.h>
#include <string.h>

extern char g_log_path[512];
extern int  g_enable_logging;

#define write_log(path, fmt, ...) do { \
    (void)(path); \
    fprintf(stderr, "elf2fself: " fmt "\n", ##__VA_ARGS__); \
} while (0)

/* Stub — on-screen toast not needed for batch tool. */
#define printf_notification(fmt, ...) do { \
    fprintf(stderr, "notify: " fmt "\n", ##__VA_ARGS__); \
} while (0)

#endif
