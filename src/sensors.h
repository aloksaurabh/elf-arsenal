#pragma once

#include <microhttpd.h>

enum MHD_Result sensors_request(struct MHD_Connection *conn);
