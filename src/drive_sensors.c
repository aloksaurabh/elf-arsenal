
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/disk.h>
#include <sys/statvfs.h>

#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_pass.h>

#include <microhttpd.h>

#include "drive_sensors.h"
#include "third_party/cJSON.h"
#include "websrv.h"

#define MAX_DA_DEVS     10
#define LOG_SENSE_TEMP  0x0D
#define LOG_BUF_LEN     64
#define CAM_TIMEOUT_MS  5000

/* Send LOG SENSE page 0x0D (Temperature) via CAMIOCOMMAND on an open fd.
   Returns temperature in Celsius, or negative on any failure. */
static int
read_scsi_temp(int fd)
{
    uint8_t buf[LOG_BUF_LEN];
    memset(buf, 0, sizeof(buf));

    union ccb ccb;
    memset(&ccb, 0, sizeof(ccb));

    ccb.ccb_h.func_code = XPT_SCSI_IO;
    ccb.ccb_h.flags     = CAM_DIR_IN;
    ccb.ccb_h.timeout   = CAM_TIMEOUT_MS;

    ccb.csio.data_ptr   = buf;
    ccb.csio.dxfer_len  = LOG_BUF_LEN;
    ccb.csio.sense_len  = 32;
    ccb.csio.cdb_len    = 10;
    ccb.csio.tag_action = CAM_TAG_ACTION_NONE;

    /* LOG SENSE CDB (10-byte) */
    uint8_t *cdb = ccb.csio.cdb_io.cdb_bytes;
    cdb[0] = LOG_SENSE;                              /* opcode 0x4D    */
    cdb[1] = 0;                                      /* SP=0           */
    cdb[2] = SLS_PAGE_CTRL_CUMULATIVE | LOG_SENSE_TEMP; /* page 0x0D   */
    cdb[3] = 0;                                      /* subpage 0      */
    cdb[4] = 0;                                      /* reserved       */
    cdb[5] = 0;                                      /* param ptr hi   */
    cdb[6] = 0;                                      /* param ptr lo   */
    cdb[7] = 0;                                      /* alloc len hi   */
    cdb[8] = LOG_BUF_LEN;                            /* alloc len lo   */
    cdb[9] = 0;                                      /* control        */

    if (ioctl(fd, CAMIOCOMMAND, &ccb) < 0)
        return -1;
    if ((ccb.ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP)
        return -2;

    /* Temperature log page response:
     *   [0] page code 0x0D
     *   [2-3] page length
     *   [4-5] param code 0x0000
     *   [6] flags
     *   [7] param length = 2
     *   [8] reserved
     *   [9] temperature in Celsius (0xFF = not available)
     */
    if (buf[0] != LOG_SENSE_TEMP)
        return -3;
    if (buf[9] == 0xFF)
        return -4;  /* drive reports temp not available */

    return (int)(uint8_t)buf[9];
}

enum MHD_Result
drive_sensors_request(struct MHD_Connection *conn)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", 1);
    cJSON *drives = cJSON_CreateArray();

    for (int i = 0; i < MAX_DA_DEVS; i++) {
        char devpath[32];
        snprintf(devpath, sizeof(devpath), "/dev/da%d", i);

        int fd = open(devpath, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            if (errno != ENOENT)
                /* Device node exists but access denied — still report it */
                goto report_access_err;
            continue;
        }

        /* Confirm it's a real block device with DIOCGMEDIASIZE */
        off_t media_size = 0;
        if (ioctl(fd, DIOCGMEDIASIZE, &media_size) < 0) {
            close(fd);
            continue;
        }

        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "device", devpath);
        cJSON_AddNumberToObject(d, "sizeBytes", (double)media_size);

        /* Disk identifier string (vendor/model/serial from DIOCGIDENT) */
        char ident[DISK_IDENT_SIZE];
        memset(ident, 0, sizeof(ident));
        if (ioctl(fd, DIOCGIDENT, ident) == 0 && ident[0] != '\0')
            cJSON_AddStringToObject(d, "ident", ident);
        else
            cJSON_AddNullToObject(d, "ident");

        /* Temperature via SCSI LOG SENSE */
        int temp = read_scsi_temp(fd);
        if (temp >= 0)
            cJSON_AddNumberToObject(d, "tempC", temp);
        else {
            cJSON_AddNullToObject(d, "tempC");
            cJSON_AddNumberToObject(d, "tempErr", temp);
        }

        close(fd);

        /* Filesystem usage: da0→/mnt/ext0, da1→/mnt/ext1, etc. */
        {
            char mnt[32];
            snprintf(mnt, sizeof(mnt), "/mnt/ext%d", i);
            struct statvfs sv;
            if (statvfs(mnt, &sv) == 0 && sv.f_blocks > 0) {
                uint64_t total = (uint64_t)sv.f_blocks * sv.f_frsize;
                uint64_t free  = (uint64_t)sv.f_bfree  * sv.f_frsize;
                uint64_t used  = total - free;
                cJSON_AddNumberToObject(d, "fsTotalBytes", (double)total);
                cJSON_AddNumberToObject(d, "fsUsedBytes",  (double)used);
                cJSON_AddNumberToObject(d, "fsFreeBytes",  (double)free);
            }
        }

        cJSON_AddItemToArray(drives, d);
        continue;

report_access_err:
        {
            cJSON *d2 = cJSON_CreateObject();
            cJSON_AddStringToObject(d2, "device", devpath);
            cJSON_AddBoolToObject(d2, "accessDenied", 1);
            cJSON_AddItemToArray(drives, d2);
        }
    }

    /* Fixed PS5 storage mounts: internal + M.2 */
    static const struct { const char *label; const char *path; } fixed[] = {
        { "Internal SSD", "/user" },
        { "M.2 Expansion", "/mnt/ext1" },
    };
    cJSON *fixed_arr = cJSON_CreateArray();
    for (int j = 0; j < (int)(sizeof(fixed)/sizeof(fixed[0])); j++) {
        struct statvfs sv;
        if (statvfs(fixed[j].path, &sv) != 0 || sv.f_blocks == 0) continue;
        uint64_t total = (uint64_t)sv.f_blocks * sv.f_frsize;
        uint64_t free  = (uint64_t)sv.f_bfree  * sv.f_frsize;
        uint64_t used  = total - free;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "label",         fixed[j].label);
        cJSON_AddNumberToObject(e, "fsTotalBytes",  (double)total);
        cJSON_AddNumberToObject(e, "fsUsedBytes",   (double)used);
        cJSON_AddNumberToObject(e, "fsFreeBytes",   (double)free);
        cJSON_AddItemToArray(fixed_arr, e);
    }
    cJSON_AddItemToObject(root, "storage", fixed_arr);

    cJSON_AddItemToObject(root, "drives", drives);

    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    struct MHD_Response *r =
        MHD_create_response_from_buffer(strlen(txt), txt, MHD_RESPMEM_MUST_FREE);
    MHD_add_response_header(r, MHD_HTTP_HEADER_CONTENT_TYPE, "application/json");
    MHD_add_response_header(r, MHD_HTTP_HEADER_CACHE_CONTROL, "no-cache");
    enum MHD_Result rc = websrv_queue_response(conn, 200, r);
    MHD_destroy_response(r);
    return rc;
}
