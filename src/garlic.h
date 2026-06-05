
#pragma once

#include <microhttpd.h>

enum MHD_Result garlic_request(struct MHD_Connection *conn, const char *url);
