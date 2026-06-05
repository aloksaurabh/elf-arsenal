#pragma once

/* Repair PS4 fpkg (fake-package) listings in the PS5 application database.

   After Sony's safe-mode "Rebuild Database", fpkg titles vanish from app.db
   because they aren't in Sony's authoritative registration sources. The
   extracted app survives in /user/app though, so we re-create the rows a
   real install would have written, purely from on-disk data (param.sfo +
   appmeta art + pkg size) — no PSN/network. Uses the cid:local concept path
   (conceptId=0), matching offline-installed games.

   Idempotent: titles already present in tbl_contentinfo are skipped, so it
   is safe to run on every self-healer sweep.

   Returns 0 on success (DB opened + scanned), -1 on hard error (couldn't
   open app.db). *out_added receives the number of titles inserted. */
int fpkg_db_repair(int *out_added);

/* Like fpkg_db_repair but choose which backup set to restore from:
   use_prev=0 → current, use_prev=1 → 3gamesback (rollback). */
int fpkg_db_repair_src(int use_prev, int *out_added);

/* Snapshot the system's own launchable rows for every fpkg (CUSA) title now
   in app.db into /data/fpkg_backup (which survives a DB rebuild). This is the
   reliable source for repair: synthesis can't reproduce the system-computed
   installed size, but a verbatim backup row launches. Whole-DB online copy,
   guarded so a wiped post-rebuild DB can't destroy a good backup.
   *out_titles receives the number of fpkg titles captured. Returns 0 on ok. */
int fpkg_backup_sync(int *out_titles);

/* chmod 0555 all CUSA app folders on internal + USB + M.2. Folders survive a
   Sony DB rebuild; unlock before installing, moving, or deleting a game. */
void fpkg_protect_files(void);

/* Release all CUSA folders back to 0777 for install / move / delete. */
void fpkg_unprotect_files(void);

/* Returns 1 if at least one CUSA folder on any drive is currently locked. */
int fpkg_is_protected(void);

/* Remove orphaned move-source copies: a CUSA app folder that is NOT the title's
   registered location while the registered location holds a real pkg (the 0555
   lock blocked the move's own source delete). Run AFTER fpkg_db_repair so a
   stale registration is corrected first. Returns the number of orphans
   removed (or, in dry-run, detected). Logs every target. */
int fpkg_sweep_orphans(void);

/* Take a full snapshot of the live app.db + appinfo.db.
   Rotates the previous current → 1installback, then copies the live DBs into
   current. Called automatically on new installs; also triggerable from the UI.
   Returns 0 on success, -1 if either copy failed. */
int fpkg_db_full_snapshot(void);

/* Restore the live app.db + appinfo.db from a snapshot slot.
   slot = "current", "1installback", or "manual/YYYY-MM-DD_HH-MM-SS".
   Returns 0 on success, -1 if the slot has no snapshot or copy failed.
   Caller should call sys_refresh_shellui() after this returns 0. */
int fpkg_db_full_restore(const char *slot);

/* Return a JSON array of available automatic snapshot slot names (malloc'd).
   Example: ["current","1installback"] */
char *fpkg_db_snapshot_list(void);

/* Save a manual dated snapshot to snap/manual/YYYY-MM-DD_HH-MM-SS/.
   Does NOT touch the automatic current/1installback slots.
   Returns 0 on success. */
int fpkg_db_manual_snapshot(void);

/* Return a JSON array of manual snapshot names, newest first (malloc'd).
   Example: ["2026-05-29_07-00-00","2026-05-29_06-00-00"] */
char *fpkg_db_manual_list(void);
