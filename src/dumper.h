
#pragma once

#include <microhttpd.h>

enum MHD_Result dumper_request(struct MHD_Connection *conn, const char *url);

void dumper_seed_configs(void);
