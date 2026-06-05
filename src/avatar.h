
#pragma once

#include <microhttpd.h>
#include <stddef.h>

enum MHD_Result avatar_request(struct MHD_Connection *conn, const char *url);

enum MHD_Result avatar_upload_request(struct MHD_Connection *conn,
                                      const char *upload_data,
                                      size_t *upload_data_size,
                                      void **state);

/* Connection-end cleanup if the browser bails mid-upload. */
void avatar_upload_free(void *state);
