
#pragma once

#include <microhttpd.h>

enum MHD_Result kstuff_updater_request(struct MHD_Connection *conn,
                                       const char *url);

int kstuff_install_direct(int use_drakmor);
