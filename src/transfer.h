
#pragma once

#include <microhttpd.h>

/* Routes /api/fs/... — list / usb / copy / move / delete / mkdir /
   rename / job/status / job/cancel. */
enum MHD_Result transfer_request(struct MHD_Connection *conn, const char *url);

enum MHD_Result fs_upload_request(struct MHD_Connection *conn,
                                  const char *upload_data,
                                  size_t *upload_data_size,
                                  void **state);
void fs_upload_free(void *state);
