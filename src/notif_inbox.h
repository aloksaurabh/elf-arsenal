#pragma once

#include <microhttpd.h>

void              notif_inbox_init(void);

void              notif_inbox_push(const char *msg);
enum MHD_Result   notif_inbox_request(struct MHD_Connection *c, const char *url);
