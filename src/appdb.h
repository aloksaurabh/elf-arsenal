/* Elf Arsenal — app.db browsing endpoints. */

#pragma once

#include <microhttpd.h>

enum MHD_Result appdb_request(struct MHD_Connection *conn, const char *url);
