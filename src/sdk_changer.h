/* Elf Arsenal — sdk-changer.elf dispatcher (see sdk_changer.c header
   comment for endpoint list). */

#pragma once

#include <microhttpd.h>

enum MHD_Result sdk_changer_request(struct MHD_Connection *conn,
                                    const char *url);
