
#pragma once

#include <microhttpd.h>
#include <stddef.h>

enum MHD_Result homebrew_request(struct MHD_Connection *conn, const char *url);

void homebrew_wipe_staged_pkgs(void);

void homebrew_auto_install_tile_init(void);

int  homebrew_tile_autoinstall_enabled(void);
void homebrew_tile_autoinstall_set_enabled(int on);

enum MHD_Result pkg_upload_request(struct MHD_Connection *conn,
                                   const char *upload_data,
                                   size_t *upload_data_size,
                                   void **state);
void pkg_upload_free(void *state);
