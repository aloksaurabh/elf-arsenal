#pragma once

#include <microhttpd.h>

// Web endpoints. Routed by /api/y2jb prefix in websrv.c.
enum MHD_Result y2jb_request(struct MHD_Connection *conn, const char *url);

void y2jb_startup_init(void);
