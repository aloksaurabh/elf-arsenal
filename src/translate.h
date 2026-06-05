/* Translate proxy + cache for the Elf Arsenal web UI.
 *
 * The web UI calls POST /api/translate?to=<lang>&q=<text>
 * and we proxy to Google Translate's free `gtx` endpoint, extracting
 * the translated string and returning JSON {"ok":1,"t":"..."}.
 *
 * Disk cache: /data/elf-arsenal/i18n/<lang>.json — a flat
 * {english: translated} map populated lazily as the UI requests strings.
 */

#pragma once

#include <microhttpd.h>

enum MHD_Result translate_request(struct MHD_Connection *conn, const char *url);
