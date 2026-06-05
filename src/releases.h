
#pragma once

#include <microhttpd.h>

enum MHD_Result releases_request(struct MHD_Connection *conn, const char *url);

void releases_init(void);
