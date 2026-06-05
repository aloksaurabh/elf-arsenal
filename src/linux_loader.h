#pragma once

#include <microhttpd.h>

void linux_loader_init(void);

/* True if the firmware allows the linux JB chain (FW <= 6.02). */
int  linux_loader_supported(void);

/* True if /data/elf-arsenal/dl/ps5-linux-loader.elf is present + non-empty. */
int  linux_loader_is_installed(void);

enum MHD_Result linux_loader_request(struct MHD_Connection *conn,
                                     const char *url, const char *method);
