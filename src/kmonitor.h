
#pragma once

void kmonitor_start(void);

/* Settings API used by the web UI's /api/state endpoint. All return
   functions are safe to call from any thread. */
int  kmonitor_kstuff_supported(void);
int  kmonitor_kstuff_is_enabled(void);          /* 1=on, 0=off, -1=unknown */
int  kmonitor_kstuff_set(int on);                /* 0=success, -1=failure */

int  kmonitor_auto_toggle_enabled(void);
void kmonitor_set_auto_toggle(int on);

void kmonitor_get_delays(int *pause_seconds, int *resume_seconds);
void kmonitor_set_delays(int pause_seconds, int resume_seconds);
