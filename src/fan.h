/* Elf Arsenal — fan threshold control. */

#pragma once

#include <microhttpd.h>

enum MHD_Result fan_request(struct MHD_Connection *conn, const char *url);

void fan_init(void);

/* Get / set the current pinned threshold. Used by the persistent
   config (config.c) so the value survives a payload redeploy. */
int  fan_pinned_threshold(void);
void fan_pin_threshold(int temp_c);
