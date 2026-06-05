#pragma once
#include <microhttpd.h>

#define XML_PATCHES_DIR  "/data/elf-arsenal/patches/xml"
#define XML_PATCHES_OFF  "/data/elf-arsenal/patches/xml.off"

int   xml_patches_global_enabled(void);
void  xml_patches_set_global(int on);
char *xml_patches_list_json(void);
int   xml_patches_toggle(const char *name, int on, int dir_idx);
int   xml_patches_delete(const char *name, int dir_idx);

enum MHD_Result xml_patches_request(struct MHD_Connection *conn, const char *url);
enum MHD_Result xml_patches_upload_request(struct MHD_Connection *conn,
                                           const char *upload_data,
                                           size_t *upload_data_size,
                                           void **state);
