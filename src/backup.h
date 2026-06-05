
#pragma once

#include <microhttpd.h>


void backup_init(void);


/* Spawn the dedicated backup-worker thread. MUST be called from main() */
int backup_worker_init(void);


int backup_snapshot_file(const char *tag, const char *src_path);


/* Recursively copy `src_dir` into the current snapshot under `tag`.
   Same single-second-coalescing semantics as backup_snapshot_file. */
int backup_snapshot_tree(const char *tag, const char *src_dir);


int backup_dump_registry(void);


/* HTTP dispatcher — handles /api/backup/{list,dump-registry,
   restore,delete}. */
enum MHD_Result backup_request(struct MHD_Connection *conn, const char *url);


int  backup_is_enabled(void);
void backup_set_enabled(int on);
