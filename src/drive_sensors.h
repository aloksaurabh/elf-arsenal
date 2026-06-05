#pragma once
#include <microhttpd.h>

enum MHD_Result drive_sensors_request(struct MHD_Connection *conn);
