#pragma once

#include <stddef.h>
#include <stdint.h>

#include <microhttpd.h>

void offline_pack_init(void);

int  offline_pack_get_enabled(void);
void offline_pack_set_enabled(int on);
int  offline_pack_is_installed(void);

int offline_pack_get_release_list(const char *tool,
                                  uint8_t **out_buf, size_t *out_len);

int offline_pack_get_asset(const char *tool, const char *tag,
                           const char *asset,
                           uint8_t **out_buf, size_t *out_len);

enum MHD_Result offline_pack_request(struct MHD_Connection *conn,
                                     const char *url);
