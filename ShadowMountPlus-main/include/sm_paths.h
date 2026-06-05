#ifndef SM_PATHS_H
#define SM_PATHS_H

#define IMAGE_MOUNT_BASE "/mnt/shadowmnt"

#define DEFAULT_BACKPORTS_DIR_NAME "backports"
#define LOG_DIR "/data/shadowmount"
#define LOG_FILE "/data/shadowmount/debug.log"
#define LOG_FILE_PREV "/data/shadowmount/debug.log.1"
#define CONFIG_FILE "/data/shadowmount/config.ini"
#define AUTOTUNE_FILE "/data/shadowmount/autotune.ini"
#define APPMETA_BASE "/user/appmeta"
#define APP_BASE "/user/app"
#define KSTUFF_NOAUTOMOUNT_FILE "/data/.kstuff_noautomount"
#define KILL_FILE "/data/shadowmount/STOP"
#define TOAST_FILE "/data/shadowmount/notify.txt"
#define NOTIFY_ICON_DIR "/user/data/shadowmount"
#define NOTIFY_ICON_FILE "/user/data/shadowmount/smp_icon.png"
#define APP_DB_PATH "/system_data/priv/mms/app.db"

/* Compile-time default scan roots — restored to drakmor's original
   list. Sonic Loader doesn't bundle SMP anymore (users install via the
   release picker), but we keep this header in sync with upstream so the
   source tree is honest. The Sonic Loader settings UI manages whether
   these defaults are honored (toggle) and any user-added manual paths,
   purely by editing /data/shadowmount/config.ini's scanpath= lines —
   when at least one scanpath= entry is present, SMP ignores the
   compile-time defaults; that's the lever the toggle pulls. */
#define SM_DEFAULT_SCAN_PATHS_INITIALIZER                                      \
  {                                                                            \
    /* Internal */                                                             \
    "/data/homebrew", "/data/etaHEN/games",                                   \
    /* Extended Storage */                                                     \
    "/mnt/ext0/homebrew", "/mnt/ext0/etaHEN/games",                           \
    /* M.2 Drive */                                                            \
    "/mnt/ext1/homebrew", "/mnt/ext1/etaHEN/games",                           \
    /* USB Subfolders */                                                       \
    "/mnt/usb0/homebrew", "/mnt/usb1/homebrew", "/mnt/usb2/homebrew",         \
        "/mnt/usb3/homebrew", "/mnt/usb4/homebrew", "/mnt/usb5/homebrew",     \
        "/mnt/usb6/homebrew", "/mnt/usb7/homebrew",                           \
        "/mnt/usb0/etaHEN/games", "/mnt/usb1/etaHEN/games",                   \
        "/mnt/usb2/etaHEN/games", "/mnt/usb3/etaHEN/games",                   \
        "/mnt/usb4/etaHEN/games", "/mnt/usb5/etaHEN/games",                   \
        "/mnt/usb6/etaHEN/games", "/mnt/usb7/etaHEN/games",                   \
    /* USB Root Paths */                                                       \
    "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3", "/mnt/usb4",          \
        "/mnt/usb5", "/mnt/usb6", "/mnt/usb7", "/mnt/ext0", "/mnt/ext1", NULL \
  }

#endif
