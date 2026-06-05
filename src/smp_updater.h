
#pragma once

#include <microhttpd.h>

enum MHD_Result smp_updater_request(struct MHD_Connection *conn,
                                    const char *url);

int smp_install_direct(const char *version);

int sys_smp_set_enabled(int on);
int sys_smp_is_running(void);
/* Call cb(path, arg) for every active SMP scan path (defaults + manual). */
void smp_foreach_scan_path(void (*cb)(const char *, void *), void *arg);

/* SMP config.ini tunable getters/setters (persisted via config_save). */
int  smp_cfg_get_debug(void);
void smp_cfg_set_debug(int v);
int  smp_cfg_get_quiet_mode(void);
void smp_cfg_set_quiet_mode(int v);
int  smp_cfg_get_kstuff_auto_toggle(void);
void smp_cfg_set_kstuff_auto_toggle(int v);
int  smp_cfg_get_kstuff_crash_detection(void);
void smp_cfg_set_kstuff_crash_detection(int v);
int  smp_cfg_get_pause_delay_image(void);
void smp_cfg_set_pause_delay_image(int v);
int  smp_cfg_get_pause_delay_direct(void);
void smp_cfg_set_pause_delay_direct(int v);
/* Restart the daemon — used after the scan-path config changes so the
   new paths are picked up. Returns 0 on success, -1 on error. */
int sys_smp_restart(void);

/* Start the background wake-watchdog thread that detects console resume
   from standby and aggressively restarts SMP to clear stale state. */
void sys_start_wake_watchdog(void);
