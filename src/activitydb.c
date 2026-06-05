
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>

#include <microhttpd.h>
#include <sqlite3.h>

#include "activitydb.h"
#include "websrv.h"


#define SL2_DB_PATH \
  "/system_data/priv/system_logger2/nobackup/database/sl2_log.db"

/* Hardcoded test title for now. */
#define TEST_TITLE_ID  "PPSA08804"


static enum MHD_Result
serve_buf(struct MHD_Connection *conn, unsigned int status,
          const char *mime, void *data, size_t size, int free_after) {
  enum MHD_Result ret = MHD_NO;
  struct MHD_Response *resp;
  enum MHD_ResponseMemoryMode mode = free_after ? MHD_RESPMEM_MUST_FREE
                                                : MHD_RESPMEM_PERSISTENT;
  if((resp = MHD_create_response_from_buffer(size, data, mode))) {
    if(mime)
      MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, mime);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-cache");
    ret = websrv_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
  } else if(free_after) {
    free(data);
  }
  return ret;
}


static enum MHD_Result
activitydb_query(struct MHD_Connection *conn) {
  sqlite3 *db = NULL;
  sqlite3_stmt *st = NULL;
  char body[512];
  
  const char *table = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "table");
  if(!table || strncmp(table, "tbl_iconinfo_", 13) != 0) {
    const char *err = "{\"ok\":false,\"error\":\"invalid table parameter\"}";
    return serve_buf(conn, MHD_HTTP_BAD_REQUEST, "application/json", (void*)err, strlen(err), 0);
  }

  if(sqlite3_open_v2("/system_data/priv/mms/app.db", &db,
                     SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX,
                     NULL) != SQLITE_OK) {
    int len = snprintf(body, sizeof(body),
        "{\"ok\":false,\"error\":\"Unable to open app.db: %s\"}",
        db ? sqlite3_errmsg(db) : "unknown");
    if(db) sqlite3_close(db);
    char *out = malloc(len + 1);
    if(!out) return MHD_NO;
    memcpy(out, body, len + 1);
    return serve_buf(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                     "application/json", out, len, 1);
  }

  sqlite3_busy_timeout(db, 3000);

  char sql[512];
  snprintf(sql, sizeof(sql),
    "SELECT titleId, playedTimeOnConsole, lastAccessTime, titleName FROM %s "
    "WHERE (titleId LIKE '%%PPSA%%' OR titleId LIKE '%%CUSA%%') "
    "AND titleId != 'PPSA01650' "
    "AND playedTimeOnConsole IS NOT NULL AND playedTimeOnConsole > 0 "
    "ORDER BY lastAccessTime DESC LIMIT 3", table);

  if(sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
    int len = snprintf(body, sizeof(body),
        "{\"ok\":false,\"error\":\"prepare: %s\"}",
        sqlite3_errmsg(db));
    sqlite3_close(db);
    char *out = malloc(len + 1);
    if(!out) return MHD_NO;
    memcpy(out, body, len + 1);
    return serve_buf(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                     "application/json", out, len, 1);
  }

  char recent_json[2048] = "[";
  int found_count = 0;

  sqlite3 *sl2_db = NULL;
  sqlite3_open_v2("/system_data/priv/system_logger2/nobackup/database/sl2_log.db", &sl2_db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL);

  while(sqlite3_step(st) == SQLITE_ROW) {
    const char *tid = (const char *)sqlite3_column_text(st, 0);
    int totalFgTime = sqlite3_column_int(st, 1);
    const char *cd = (const char *)sqlite3_column_text(st, 2);
    const char *tn = (const char *)sqlite3_column_text(st, 3);
    
    if(!tid) continue;

    int lastSessionTime = 0;
    int sumFgTime = 0;
    if(sl2_db) {
      sqlite3_stmt *st2 = NULL;
      const char *sql2 = "SELECT log FROM tbl_log WHERE event_id = 'ApplicationSessionEndBi' AND log LIKE ? ORDER BY created_date DESC";
      if(sqlite3_prepare_v2(sl2_db, sql2, -1, &st2, NULL) == SQLITE_OK) {
        char like[128];
        snprintf(like, sizeof(like), "%%\"appTitleId\":\"%s\"%%", tid);
        sqlite3_bind_text(st2, 1, like, -1, SQLITE_TRANSIENT);
        int is_first = 1;
        while(sqlite3_step(st2) == SQLITE_ROW) {
          const char *log = (const char *)sqlite3_column_text(st2, 0);
          const char *fg_ptr = log ? strstr(log, "\"totalFgTime\":") : NULL;
          if(fg_ptr) {
            int t = atoi(fg_ptr + 14);
            if(t > 0) {
              sumFgTime += t;
              if(is_first) {
                lastSessionTime = t;
                is_first = 0;
              }
            }
          }
        }
        sqlite3_finalize(st2);
      }
      totalFgTime = sumFgTime;
    }
    
    if(found_count > 0) strcat(recent_json, ",");
    
    char buf[512];
    snprintf(buf, sizeof(buf), "{\"titleId\":\"%s\",\"totalFgTime\":%d,\"lastSessionTime\":%d,\"createdDate\":\"%s\",\"titleName\":\"%s\"}", 
             tid, totalFgTime, lastSessionTime, cd ? cd : "", tn ? tn : "");
    strcat(recent_json, buf);
    
    found_count++;
  }
  strcat(recent_json, "]");
  
  if(sl2_db) sqlite3_close(sl2_db);
  sqlite3_finalize(st);
  sqlite3_close(db);

  char body_final[4096];
  int len = snprintf(body_final, sizeof(body_final),
      "{\"ok\":true,\"recent\":%s}", recent_json);
  char *out = malloc(len + 1);
  if(!out) return MHD_NO;
  memcpy(out, body_final, len + 1);
  return serve_buf(conn, MHD_HTTP_OK, "application/json", out, len, 1);
}


static enum MHD_Result
activitydb_users(struct MHD_Connection *conn) {
  sqlite3 *db = NULL;
  sqlite3_open_v2("/system_data/priv/mms/app.db", &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL);
  sqlite3_stmt *st = NULL;
  const char *sql = "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl_iconinfo_%' ORDER BY name ASC";
  sqlite3_prepare_v2(db, sql, -1, &st, NULL);
  char tables[16][64] = {0};
  int table_count = 0;
  while(sqlite3_step(st) == SQLITE_ROW && table_count < 16) {
    const char *n = (const char *)sqlite3_column_text(st, 0);
    if(n) strncpy(tables[table_count++], n, 63);
  }
  if(st) sqlite3_finalize(st);
  if(db) sqlite3_close(db);

  DIR *d = opendir("/user/home");
  struct dirent *dir;
  char users[16][64] = {0};
  char names[16][64] = {0};
  int user_count = 0;
  if(d) {
    while((dir = readdir(d)) != NULL && user_count < 16) {
      if(dir->d_type == DT_DIR && dir->d_name[0] != '.') {
        strncpy(users[user_count], dir->d_name, 63);
        char path[256];
        snprintf(path, sizeof(path), "/user/home/%s/username.dat", dir->d_name);
        FILE *f = fopen(path, "rb");
        if(f) {
          size_t n = fread(names[user_count], 1, 63, f);
          names[user_count][n] = 0;
          for(int i = n-1; i >= 0; i--) {
            if(names[user_count][i] == '\n' || names[user_count][i] == '\r') names[user_count][i] = 0;
          }
          fclose(f);
        } else {
          strcpy(names[user_count], "Unknown");
        }
        user_count++;
      }
    }
    closedir(d);
  }
  
  for(int i = 0; i < user_count - 1; i++) {
    for(int j = i + 1; j < user_count; j++) {
      if(strcmp(users[i], users[j]) > 0) {
        char tmp[64];
        strcpy(tmp, users[i]); strcpy(users[i], users[j]); strcpy(users[j], tmp);
        strcpy(tmp, names[i]); strcpy(names[i], names[j]); strcpy(names[j], tmp);
      }
    }
  }

  char body[4096] = "[";
  for(int i = 0; i < user_count; i++) {
    const char *tbl = (i < table_count) ? tables[i] : "";
    if(i > 0) strcat(body, ",");
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"id\":\"%s\",\"name\":\"%s\",\"table\":\"%s\"}", users[i], names[i], tbl);
    strcat(body, buf);
  }
  strcat(body, "]");

  char *out = strdup(body);
  if(!out) return MHD_NO;
  return serve_buf(conn, MHD_HTTP_OK, "application/json", out, strlen(out), 1);
}

static enum MHD_Result
activitydb_avatar(struct MHD_Connection *conn) {
  const char *id = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "id");
  if(id) {
    char path[256];
    char upper_id[64] = {0};
    strncpy(upper_id, id, 63);
    for(int i=0; upper_id[i]; i++) upper_id[i] = toupper((unsigned char)upper_id[i]);
    snprintf(path, sizeof(path), "/system_data/priv/cache/profile/0x%s/avatar.png", upper_id);
    
    FILE *f = fopen(path, "rb");
    if(f) {
      fseek(f, 0, SEEK_END);
      long size = ftell(f);
      fseek(f, 0, SEEK_SET);
      if(size > 0) {
        void *data = malloc(size);
        if(data) {
          fread(data, 1, size, f);
          fclose(f);
          return serve_buf(conn, MHD_HTTP_OK, "image/png", data, size, 1);
        }
      }
      fclose(f);
    }
  }
  const char *err = "Not found";
  return serve_buf(conn, MHD_HTTP_NOT_FOUND, "text/plain", (void*)err, strlen(err), 0);
}

static enum MHD_Result
activitydb_game(struct MHD_Connection *conn) {
  const char *titleId = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "titleId");
  if(!titleId || strlen(titleId) == 0) {
    const char *err = "{\"ok\":false,\"error\":\"missing titleId\"}";
    return serve_buf(conn, MHD_HTTP_BAD_REQUEST, "application/json", (void*)err, strlen(err), 0);
  }

  sqlite3 *db = NULL;
  sqlite3_stmt *st = NULL;
  
  if(sqlite3_open_v2("/system_data/priv/system_logger2/nobackup/database/sl2_log.db", &db,
                     SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
    const char *err = "{\"ok\":false,\"error\":\"unable to open sl2_log.db\"}";
    if(db) sqlite3_close(db);
    return serve_buf(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "application/json", strdup(err), strlen(err), 1);
  }
  
  sqlite3_busy_timeout(db, 3000);
  
  const char *sql = "SELECT created_date, log FROM tbl_log WHERE event_id = 'ApplicationSessionEndBi' AND log LIKE ? ORDER BY created_date ASC LIMIT 1000";
  
  if(sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
    const char *err = "{\"ok\":false,\"error\":\"prepare failed\"}";
    sqlite3_close(db);
    return serve_buf(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "application/json", strdup(err), strlen(err), 1);
  }
  
  char like_pattern[128];
  snprintf(like_pattern, sizeof(like_pattern), "%%\"appTitleId\":\"%s\"%%", titleId);
  sqlite3_bind_text(st, 1, like_pattern, -1, SQLITE_TRANSIENT);
  
  size_t capacity = 32768;
  char *json_out = malloc(capacity);
  if(!json_out) {
    sqlite3_finalize(st);
    sqlite3_close(db);
    return MHD_NO;
  }
  
  /* Fetch InstalledDate and lastPlayedDate from app.db */
  char installTime[64] = "";
  char lastPlayedDate[64] = "";
  const char *table = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "table");
  if(table && strncmp(table, "tbl_iconinfo_", 13) == 0) {
    sqlite3 *app_db = NULL;
    if(sqlite3_open_v2("/system_data/priv/mms/app.db", &app_db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) == SQLITE_OK) {
      sqlite3_stmt *st_app = NULL;
      char sql_app[256];
      snprintf(sql_app, sizeof(sql_app), "SELECT InstalledDate, lastPlayedDate FROM %s WHERE titleId = ? LIMIT 1", table);
      if(sqlite3_prepare_v2(app_db, sql_app, -1, &st_app, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st_app, 1, titleId, -1, SQLITE_TRANSIENT);
        if(sqlite3_step(st_app) == SQLITE_ROW) {
          const char *it = (const char *)sqlite3_column_text(st_app, 0);
          const char *lp = (const char *)sqlite3_column_text(st_app, 1);
          if(it) strncpy(installTime, it, 63);
          if(lp) strncpy(lastPlayedDate, lp, 63);
        }
        sqlite3_finalize(st_app);
      }
      sqlite3_close(app_db);
    }
  }

  strcpy(json_out, "{\"ok\":true,\"installTime\":\"");
  strcat(json_out, installTime);
  strcat(json_out, "\",\"lastPlayedDate\":\"");
  strcat(json_out, lastPlayedDate);
  strcat(json_out, "\",\"sessions\":[");
  int count = 0;
  
  while(sqlite3_step(st) == SQLITE_ROW) {
    const char *cd = (const char *)sqlite3_column_text(st, 0);
    const char *log = (const char *)sqlite3_column_text(st, 1);
    if(!cd || !log) continue;
    
    const char *fg_ptr = strstr(log, "\"totalFgTime\":");
    int total_fg = 0;
    if(fg_ptr) {
      fg_ptr += 14;
      total_fg = atoi(fg_ptr);
    }
    
    if(strlen(json_out) + 256 > capacity) {
      capacity *= 2;
      char *tmp = realloc(json_out, capacity);
      if(!tmp) break;
      json_out = tmp;
    }
    
    if(count > 0) strcat(json_out, ",");
    char row_buf[128];
    snprintf(row_buf, sizeof(row_buf), "{\"date\":\"%s\",\"totalFgTime\":%d}", cd, total_fg);
    strcat(json_out, row_buf);
    
    count++;
  }
  strcat(json_out, "]}");
  
  sqlite3_finalize(st);
  sqlite3_close(db);
  
  return serve_buf(conn, MHD_HTTP_OK, "application/json", json_out, strlen(json_out), 1);
}

static enum MHD_Result
activitydb_rawlog(struct MHD_Connection *conn) {
  const char *titleId = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "titleId");
  if(!titleId || strlen(titleId) == 0) {
    const char *err = "{\"ok\":false,\"error\":\"missing titleId\"}";
    return serve_buf(conn, MHD_HTTP_BAD_REQUEST, "application/json", (void*)err, strlen(err), 0);
  }

  sqlite3 *db = NULL;
  sqlite3_stmt *st = NULL;

  if(sqlite3_open_v2("/system_data/priv/system_logger2/nobackup/database/sl2_log.db", &db,
                     SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
    const char *err = "{\"ok\":false,\"error\":\"unable to open sl2_log.db\"}";
    if(db) sqlite3_close(db);
    return serve_buf(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "application/json", strdup(err), strlen(err), 1);
  }

  sqlite3_busy_timeout(db, 3000);

  /* Return first raw log entry for this title across all session-like event_ids */
  const char *sql = "SELECT event_id, created_date, log FROM tbl_log WHERE log LIKE ? LIMIT 1";
  if(sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
    const char *err = "{\"ok\":false,\"error\":\"prepare failed\"}";
    sqlite3_close(db);
    return serve_buf(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "application/json", strdup(err), strlen(err), 1);
  }

  char like_pattern[128];
  snprintf(like_pattern, sizeof(like_pattern), "%%\"%s\"%%", titleId);
  sqlite3_bind_text(st, 1, like_pattern, -1, SQLITE_TRANSIENT);

  char *out = NULL;
  if(sqlite3_step(st) == SQLITE_ROW) {
    const char *ev  = (const char *)sqlite3_column_text(st, 0);
    const char *cd  = (const char *)sqlite3_column_text(st, 1);
    const char *log = (const char *)sqlite3_column_text(st, 2);
    /* Output as plain text so embedded JSON quotes don't break the response */
    size_t log_len = log ? strlen(log) : 0;
    size_t out_len = 64 + (ev ? strlen(ev) : 0) + (cd ? strlen(cd) : 0) + log_len + 16;
    out = malloc(out_len);
    if(out) {
      snprintf(out, out_len, "event_id: %s\ncreated_date: %s\nlog: %s\n",
        ev ? ev : "(null)", cd ? cd : "(null)", log ? log : "(null)");
    }
  } else {
    out = strdup("No log entries found for this titleId in sl2_log.db\n");
  }

  sqlite3_finalize(st);
  sqlite3_close(db);

  if(!out) return MHD_NO;
  return serve_buf(conn, MHD_HTTP_OK, "text/plain", out, strlen(out), 1);
}

enum MHD_Result
activitydb_deleted_games(struct MHD_Connection *conn) {
  sqlite3 *db = NULL;
  sqlite3_stmt *st = NULL;

  if(sqlite3_open_v2("/system_data/priv/system_logger2/nobackup/database/sl2_log.db", &db,
                     SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
    const char *err = "{\"ok\":false,\"error\":\"unable to open sl2_log.db\"}";
    if(db) sqlite3_close(db);
    return serve_buf(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "application/json", strdup(err), strlen(err), 1);
  }

  sqlite3_busy_timeout(db, 3000);

  const char *sql = "SELECT log FROM tbl_log WHERE event_id = 'ApplicationSessionEndBi'";
  if(sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
    const char *err = "{\"ok\":false,\"error\":\"prepare failed\"}";
    sqlite3_close(db);
    return serve_buf(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "application/json", strdup(err), strlen(err), 1);
  }

  sqlite3_stmt *st_name = NULL;
  const char *sql_name = "SELECT log FROM tbl_log WHERE event_id = 'ViewableImpression' AND log LIKE ? LIMIT 1";
  sqlite3_prepare_v2(db, sql_name, -1, &st_name, NULL);

  char *json_out = malloc(8192);
  if(!json_out) { sqlite3_finalize(st); if(st_name) sqlite3_finalize(st_name); sqlite3_close(db); return MHD_NO; }
  strcpy(json_out, "{\"ok\":true,\"games\":[");
  size_t capacity = 8192;
  int count = 0;

  char **seen_ids = NULL;
  int seen_count = 0;
  int seen_capacity = 0;

  while(sqlite3_step(st) == SQLITE_ROW) {
    const char *log = (const char *)sqlite3_column_text(st, 0);
    if(!log) continue;
    
    const char *id_ptr = strstr(log, "\"appTitleId\":\"");
    if(!id_ptr) continue;
    id_ptr += 14;
    
    if(strncmp(id_ptr, "PPSA", 4) != 0 && strncmp(id_ptr, "CUSA", 4) != 0) continue;
    
    char titleId[32] = {0};
    int i = 0;
    while(id_ptr[i] && id_ptr[i] != '"' && i < 31) {
      titleId[i] = id_ptr[i];
      i++;
    }
    titleId[i] = '\0';
    
    int is_duplicate = 0;
    for(int j = 0; j < seen_count; j++) {
      if(strcmp(seen_ids[j], titleId) == 0) {
        is_duplicate = 1;
        break;
      }
    }
    if(is_duplicate) continue;
    
    if(seen_count >= seen_capacity) {
      seen_capacity = seen_capacity == 0 ? 16 : seen_capacity * 2;
      seen_ids = realloc(seen_ids, seen_capacity * sizeof(char*));
    }
    seen_ids[seen_count++] = strdup(titleId);
    
    char titleName[256] = {0};
    strcpy(titleName, titleId);

    if(st_name) {
      char like_pattern[128];
      snprintf(like_pattern, sizeof(like_pattern), "%%%s%%", titleId);
      sqlite3_bind_text(st_name, 1, like_pattern, -1, SQLITE_TRANSIENT);
      if(sqlite3_step(st_name) == SQLITE_ROW) {
        const char *vlog = (const char *)sqlite3_column_text(st_name, 0);
        if(vlog) {
          const char *fg_ptr = strstr(vlog, "*FG* ");
          if(fg_ptr) {
            fg_ptr += 5;
            i = 0;
            while(fg_ptr[i] && fg_ptr[i] != '"' && i < 255) {
              if(fg_ptr[i] == '\\' && fg_ptr[i+1] == '"') {
                titleName[i] = '\\'; titleName[i+1] = '"'; i += 2; fg_ptr++;
              } else {
                titleName[i] = fg_ptr[i];
                i++;
              }
            }
            titleName[i] = '\0';
          }
        }
      }
      sqlite3_reset(st_name);
    }
    
    char game_buf[512];
    snprintf(game_buf, sizeof(game_buf), "{\"titleId\":\"%s\",\"titleName\":\"%s\"}", titleId, titleName);
    
    if(strlen(json_out) + strlen(game_buf) + 10 > capacity) {
      capacity *= 2;
      json_out = realloc(json_out, capacity);
    }
    if(count > 0) strcat(json_out, ",");
    strcat(json_out, game_buf);
    count++;
  }
  
  strcat(json_out, "]}");
  
  for(int j = 0; j < seen_count; j++) free(seen_ids[j]);
  free(seen_ids);
  
  sqlite3_finalize(st);
  if(st_name) sqlite3_finalize(st_name);
  sqlite3_close(db);
  
  return serve_buf(conn, MHD_HTTP_OK, "application/json", json_out, strlen(json_out), 1);
}

enum MHD_Result
activitydb_request(struct MHD_Connection *conn, const char *url) {
  if(!strcmp(url, "/api/activitydb")) {
    return activitydb_query(conn);
  }
  if(!strcmp(url, "/api/activitydb/users")) {
    return activitydb_users(conn);
  }
  if(!strcmp(url, "/api/activitydb/avatar")) {
    return activitydb_avatar(conn);
  }
  if(!strcmp(url, "/api/activitydb/game")) {
    return activitydb_game(conn);
  }
  if(!strcmp(url, "/api/activitydb/rawlog")) {
    return activitydb_rawlog(conn);
  }
  if(!strcmp(url, "/api/activitydb/deleted_games")) {
    return activitydb_deleted_games(conn);
  }
  const char *err = "{\"ok\":false,\"error\":\"no such endpoint\"}";
  return serve_buf(conn, MHD_HTTP_NOT_FOUND, "application/json",
                   (void*)err, strlen(err), 0);
}
