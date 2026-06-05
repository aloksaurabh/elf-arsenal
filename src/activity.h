
#pragma once

#include <microhttpd.h>

void activity_init(void);
void activity_record_launch(const char *title_id);
void activity_record_exit(const char *title_id);

enum MHD_Result activity_request(struct MHD_Connection *conn, const char *url);
