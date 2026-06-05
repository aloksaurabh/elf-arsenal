
#pragma once

#include <microhttpd.h>

enum MHD_Result cheats_request(struct MHD_Connection *conn, const char *url,
                               const char *method, const char *upload_data,
                               size_t *upload_data_size, void **con_cls);

void cheats_init(void);

/* Master enable flag — when off, no cheats are applied/reverted. */
int  cheats_engine_enabled(void);
void cheats_engine_set_enabled(int on);

int  cheats_game_running(void);

void cheats_patches_start_watcher(void);
int  cheats_patches_auto_enabled(void);
void cheats_patches_auto_set_enabled(int on);
int  cheats_patches_last_mod_count(void);   /* mods applied to last launched game */
int  cheats_patches_total_writes(void);     /* lifetime byte-stream writes */
