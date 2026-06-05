#pragma once

#include <microhttpd.h>

void              dashboards_klog_push(const char *line);
enum MHD_Result   dashboards_klogs_request(struct MHD_Connection *c, const char *url);
enum MHD_Result   dashboards_stats_request(struct MHD_Connection *c);
