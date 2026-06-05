/* fpkg-guard — standalone PS5 CUSA folder protection daemon.
 *
 * Locks CUSA app/addcont/patch/playgo folders (chmod 0555) on all three drives
 * on startup so Sony's Rebuild Database GC cannot delete them. Takes an initial
 * DB snapshot on first run; auto-snapshots after every install thereafter.
 * Serves a web UI on port 1666 for lock/unlock, manual backup, and restore. */

#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <netinet/in.h>

#include "fpkg_db.h"

#define GUARD_PORT        1666
#define APP_DB_FULLPATH   "/system_data/priv/mms/app.db"
#define RELOCK_IDLE_S     600   /* relock after 10 min of no install activity */
#define UNLOCK_FLAG_FILE  "/tmp/.fpkg_guard_unlock"

/* Nonzero while user has unlocked; updated on unlock and on install activity.
   Written by HTTP thread, read by daemon thread — volatile is sufficient. */
static volatile time_t g_last_activity = 0;

/* -------------------------------------------------- kill SceShellUI to reload */
static void refresh_shellui(void) {
  int mib[4] = {1, 14, 8, 0};
  size_t buf_size = 0;
  if(sysctl(mib, 4, NULL, &buf_size, NULL, 0)) return;
  uint8_t *buf = malloc(buf_size);
  if(!buf) return;
  if(sysctl(mib, 4, buf, &buf_size, NULL, 0)) { free(buf); return; }
  static const char *names[] = { "SceShellUI", "ShellUI", NULL };
  for(uint8_t *ptr = buf; ptr < buf + buf_size; ) {
    int ki_structsize = *(int*)ptr;
    pid_t ki_pid      = *(pid_t*)&ptr[72];
    char *ki_tdname   = (char*)&ptr[447];
    for(int i = 0; names[i]; i++) {
      if(!strcmp(ki_tdname, names[i])) {
        kill(ki_pid, SIGKILL);
        fprintf(stderr, "fpkg_guard: killed %s pid=%d\n", names[i], (int)ki_pid);
      }
    }
    ptr += ki_structsize;
  }
  free(buf);
}

/* --------------------------------------------------------- PS5 notification */
static void notify(const char *fmt, ...) {
  struct { char pad[45]; char msg[1024]; char pad2[2051]; } req;
  memset(&req, 0, sizeof req);
  va_list ap; va_start(ap, fmt);
  vsnprintf(req.msg, sizeof req.msg, fmt, ap);
  va_end(ap);
  extern int sceKernelSendNotificationRequest(int, void*, size_t, int);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
  fprintf(stderr, "fpkg_guard: %s\n", req.msg);
}

/* ---------------------------------------------------------------- HTML page */
static const char HTML[] =
"<!DOCTYPE html><html><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>FPKG Guard</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:-apple-system,sans-serif;background:#0f0f1a;color:#e0e0e0;"
"min-height:100vh;display:flex;align-items:center;justify-content:center;padding:16px}"
".card{background:#1a1a2e;border:1px solid #2a2a4e;border-radius:12px;"
"padding:28px;max-width:560px;width:100%%}"
"h1{color:#e94560;font-size:22px;margin-bottom:14px}"
".desc{color:#999;font-size:14px;line-height:1.7;margin-bottom:16px}"
".status{background:#0d0d1a;border-radius:8px;padding:12px 14px;margin-bottom:18px}"
".status-label{font-size:13px;color:#666;margin-bottom:4px}"
".locked{color:#4caf50;font-size:18px;font-weight:700}"
".unlocked{color:#e94560;font-size:18px;font-weight:700}"
".drives{color:#555;font-size:11px;margin-top:6px;line-height:1.8}"
".btns{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:10px}"
".btn{flex:1;min-width:130px;padding:12px;border-radius:8px;border:none;font-size:14px;"
"font-weight:600;cursor:pointer;text-align:center;display:block}"
".btn-lock{background:#4caf50;color:#000}"
".btn-unlock{background:#e94560;color:#fff}"
".btn-rebuild{background:#6a5cff;color:#fff}"
".btn-snap{background:linear-gradient(135deg,#1a7aff,#06e6a0);color:#fff}"
".btn-restore{background:#1a1a2e;color:#eee;border:1px solid #c0392b!important}"
".btn-msnap{background:#ff8c00;color:#000}"
".section-title{font-size:12px;color:#555;text-transform:uppercase;letter-spacing:.1em;"
"margin:16px 0 8px}"
".mrestore-wrap{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:10px;align-items:stretch}"
".msel{flex:2;min-width:160px;background:#0d0d1a;color:#e0e0e0;"
"border:1px solid #2a2a4e;border-radius:8px;padding:10px 12px;font-size:13px}"
".steps{background:#12121f;border-radius:8px;padding:12px 14px;margin-top:12px;"
"font-size:13px;color:#aaa;line-height:1.8}"
".steps ol{margin:6px 0 8px 18px}"
".steps li{margin-bottom:2px}"
".auto-note{color:#666;font-size:12px;margin-top:8px;line-height:1.7}"
".note{margin-top:14px;background:#12121f;border-radius:8px;padding:12px 14px;"
"color:#666;font-size:12px;line-height:1.7}"
".note strong{color:#aaa}"
".credit{margin-top:14px;background:#0d0d1a;border-radius:8px;padding:12px 14px;"
"color:#555;font-size:11px;line-height:1.7;border-left:3px solid #e94560}"
".credit strong{color:#999}"
"#toast{position:fixed;bottom:24px;right:20px;padding:10px 16px;border-radius:8px;"
"font-size:13px;font-weight:600;display:none;background:#1a7aff;color:#fff;"
"box-shadow:0 4px 16px rgba(0,0,0,.5);z-index:999;transition:opacity .3s}"
"#toast.ok{background:#4caf50;color:#000}"
"#toast.err{background:#e94560;color:#fff}"
"</style></head><body>"
"<div id='toast'></div>"
"<div class='card'>"
"<h1>FPKG Guard</h1>"
"<p class='desc'>"
"Locks PS4 fpkg CUSA folders on all three drives so Sony's Rebuild Database GC cannot delete them. "
"Covers <strong>app</strong>, <strong>addcont</strong> (DLC), <strong>patch</strong>, "
"and <strong>playgo</strong> on internal, /mnt/ext0, and /mnt/ext1."
"</p>"
"<div class='status'>"
"<div class='status-label'>Protection status</div>"
"<div class='%s'>%s</div>"
"<div class='drives'>"
"app &nbsp;&#183;&nbsp; addcont &nbsp;&#183;&nbsp; patch &nbsp;&#183;&nbsp; playgo<br>"
"internal &nbsp;&#183;&nbsp; /mnt/ext0 (USB) &nbsp;&#183;&nbsp; /mnt/ext1 (M.2)"
"</div>"
"</div>"
"<div class='section-title'>Protection</div>"
"<div class='btns'>"
"<button class='btn btn-lock'   onclick=\"act('/lock',  'Locking all folders')\">&#128274; Lock All</button>"
"<button class='btn btn-unlock' onclick=\"act('/unlock','Unlocking all folders')\">&#128275; Unlock All</button>"
"</div>"
"<div class='section-title'>Database</div>"
"<div class='btns'>"
"<button class='btn btn-rebuild' style='display:none' onclick=\"act('/rebuild','Rebuilding registration DB')\">&#8635; Rebuild DB</button>"
"<button class='btn btn-snap'    onclick=\"act('/snap',   'Saving snapshot')\">&#128248; Snapshot now</button>"
"</div>"
"<div class='btns'>"
"<button class='btn btn-restore' onclick=\"act('/restore?from=current',      'Restoring current snapshot')\">&#8617; Restore current</button>"
"<button class='btn btn-restore' onclick=\"act('/restore?from=1installback', 'Restoring pre-install snapshot')\">&#8617; Restore 1installback</button>"
"</div>"
"<div class='section-title'>Manual Backups</div>"
"<div class='btns'>"
"<button class='btn btn-msnap' onclick=\"act('/msnap','Saving manual backup')\">&#128190; Save Manual Backup</button>"
"</div>"
"<div class='mrestore-wrap'>"
"<select id='mslot' class='msel'><option value=''>Loading&#8230;</option></select>"
"<button class='btn btn-restore' onclick='mrestore()'>&#8617; Restore Selected</button>"
"</div>"
"<div class='steps'>"
"<strong>How to install, add DLC / patches, or move a game:</strong>"
"<ol>"
"<li>Click <strong>Unlock All</strong> to lift folder protection.</li>"
"<li>Install your PKG, DLC, patch, or move the game in PS4 Settings.</li>"
"<li>Protection auto-re-applies after <strong>10 minutes of no install activity</strong>, or click <strong>Lock All</strong> to lock immediately.</li>"
"</ol>"
"<p class='auto-note'>"
"The database is snapshotted <strong>automatically</strong> within ~60 seconds of each install "
"(detected by watching app.db for changes). "
"<em>current</em> = latest state &nbsp;&#183;&nbsp; "
"<em>1installback</em> = state right before that install.<br>"
"<strong>Snapshot now</strong> is a manual override only. "
"<strong>Restoring is always manual</strong> — never automatic."
"</p>"
"</div>"
"<div class='note'>"
"<strong>Lock/Unlock:</strong> unlock before moving, deleting, reinstalling, or installing DLC/patches.<br>"
"<strong>Rebuild DB:</strong> re-registers all PS4 fpkg titles — use after a Sony Rebuild Database.<br>"
"<strong>Snapshot now:</strong> manual override; auto-snapshots run in the background.<br>"
"<strong>Save Manual Backup:</strong> dated copy stored separately, never overwrites current/1installback.<br>"
"<strong>Restore:</strong> copies a snapshot back over the live DBs and refreshes the home screen."
"</div>"
"<div class='credit'>"
"&#10084; Thank you to <strong>Dr. Yenyen</strong> for the insight that setting a folder to "
"<strong>chmod 0555</strong> (read + execute, no write permission) prevents Sony's Rebuild Database "
"garbage collector from deleting it — the core technique this tool is built on."
"</div>"
"</div>"
"<script>"
"function act(url,label){"
"var t=document.getElementById('toast');"
"t.className='';"
"t.textContent=label+'\\u2026';"
"t.style.display='block';"
"fetch(url)"
".then(function(){"
"t.className='ok';"
"t.textContent='Done \\u2014 reloading\\u2026';"
"setTimeout(function(){location.reload();},1400);"
"})"
".catch(function(){"
"t.className='err';"
"t.textContent='Error \\u2014 check connection';"
"setTimeout(function(){t.style.display='none';},3000);"
"});}"
"function mrestore(){"
"var s=document.getElementById('mslot');"
"var v=s&&s.value;"
"if(!v)return;"
"act('/restore?from=manual/'+v,'Restoring manual backup '+v);}"
"window.addEventListener('load',function(){"
"fetch('/mlist').then(function(r){return r.json();})"
".then(function(list){"
"var s=document.getElementById('mslot');"
"if(!list||!list.length){"
"s.innerHTML=\"<option value=''>No manual backups yet</option>\";"
"}else{"
"s.innerHTML=list.map(function(n){return\"<option value='\"+n+\"'>\"+n+\"</option>\";}).join('');"
"}})"
".catch(function(){"
"document.getElementById('mslot').innerHTML=\"<option value=''>Error loading list</option>\";"
"});});"
"</script>"
"</body></html>";

/* ------------------------------------------------------------- HTTP server */
static void send_json(int fd, const char *json) {
  char hdr[160];
  int jlen = (int)strlen(json);
  int hlen = snprintf(hdr, sizeof hdr,
    "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n", jlen);
  send(fd, hdr, hlen, 0);
  send(fd, json, jlen, 0);
}

static void send_redirect(int fd, const char *loc) {
  char buf[128];
  int n = snprintf(buf, sizeof buf,
    "HTTP/1.0 302 Found\r\nLocation: %s\r\nContent-Length: 0\r\n\r\n", loc);
  send(fd, buf, n, 0);
}

static void send_page(int fd) {
  int locked = fpkg_is_protected();
  char body[sizeof(HTML) + 64];
  int blen = snprintf(body, sizeof body, HTML,
    locked ? "locked" : "unlocked",
    locked ? "&#128274; LOCKED" : "&#128275; UNLOCKED");
  char hdr[128];
  int hlen = snprintf(hdr, sizeof hdr,
    "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\n\r\n", blen);
  send(fd, hdr, hlen, 0);
  send(fd, body, blen, 0);
}

static void *guard_daemon_thread(void *arg) {
  (void)arg;
  /* Wait 60 s for the system to fully settle before doing anything.
     Touching CUSA folders or system DBs too early (while kstuff is
     mid-mount) causes a kernel panic. */
  sleep(60);

  fpkg_protect_files();
  fpkg_db_full_snapshot();
  notify("FPKG Guard: folders locked + snapshot taken — UI at http://ps5:%d", GUARD_PORT);
  fprintf(stderr, "fpkg_guard: initial protect + snapshot done\n");

  struct stat st;
  time_t last_mtime = 0;
  if(stat(APP_DB_FULLPATH, &st) == 0)
    last_mtime = st.st_mtime;
  time_t last_protect = 0;
  time_t last_backup  = time(NULL);
  int tick = 0;
  for(;;) {
    sleep(1);
    tick++;
    time_t now = time(NULL);

    /* Arsenal main process signals us to unlock via a flag file. */
    if(access(UNLOCK_FLAG_FILE, F_OK) == 0) {
      fpkg_unprotect_files();
      g_last_activity = now;
      unlink(UNLOCK_FLAG_FILE);
      notify("FPKG Guard: unlocked via Arsenal — auto-locks after 10 min idle");
      fprintf(stderr, "fpkg_guard: unlock flag received from Arsenal\n");
    }

    /* Auto-relock: only when user has unlocked AND no install activity for
       RELOCK_IDLE_S seconds. While locked, re-enforce every 30s. */
    if(g_last_activity != 0) {
      if(tick % 5 == 0 && stat(APP_DB_FULLPATH, &st) == 0 &&
         st.st_mtime != last_mtime) {
        /* app.db changed — install activity detected, reset idle timer */
        last_mtime = st.st_mtime;
        g_last_activity = now;
        if(now - last_backup >= 60) {
          fpkg_db_full_snapshot();
          last_backup = now;
          fprintf(stderr, "fpkg_guard: auto-snapshot (app.db changed)\n");
        }
      }
      if(now - g_last_activity >= RELOCK_IDLE_S) {
        fpkg_protect_files();
        g_last_activity = 0;
        last_protect = now;
        notify("FPKG Guard: auto-locked (10 min idle)");
        fprintf(stderr, "fpkg_guard: auto-relock after idle timeout\n");
      }
    } else {
      /* Locked state — re-enforce every 30s */
      if(now - last_protect >= 30) {
        fpkg_protect_files();
        last_protect = now;
      }
      if(tick % 5 == 0 && stat(APP_DB_FULLPATH, &st) == 0) {
        if(st.st_mtime != last_mtime && now - last_backup >= 60) {
          fpkg_db_full_snapshot();
          last_mtime = st.st_mtime;
          last_backup = now;
          fprintf(stderr, "fpkg_guard: auto-snapshot (app.db changed)\n");
        }
      }
    }
  }
  return NULL;
}

int main(void) {
  pthread_t dt;
  pthread_create(&dt, NULL, guard_daemon_thread, NULL);
  pthread_detach(dt);

  /* Initial protect/snapshot runs in the daemon thread after 60 s delay.
     Just announce that the guard is up and open the HTTP server. */
  notify("FPKG Guard: running — folders will be locked in ~60 s, UI at http://ps5:%d", GUARD_PORT);

  int srv = socket(AF_INET, SOCK_STREAM, 0);
  int opt = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
  struct sockaddr_in addr = {0};
  addr.sin_family      = AF_INET;
  addr.sin_port        = htons(GUARD_PORT);
  addr.sin_addr.s_addr = 0; /* INADDR_ANY */
  bind(srv, (struct sockaddr *)&addr, sizeof addr);
  listen(srv, 4);

  for(;;) {
    int cli = accept(srv, NULL, NULL);
    if(cli < 0) continue;
    char req[512] = {0};
    recv(cli, req, sizeof req - 1, 0);
    if(strstr(req, "GET /lock"))
      { fpkg_protect_files(); g_last_activity = 0; notify("FPKG Guard: locked"); send_redirect(cli, "/"); }
    else if(strstr(req, "GET /unlock"))
      { fpkg_unprotect_files(); g_last_activity = time(NULL); notify("FPKG Guard: unlocked — auto-locks after 10 min idle"); send_redirect(cli, "/"); }
    else if(strstr(req, "GET /rebuild")) {
      int n = 0; fpkg_db_repair_src(0, &n);
      fpkg_protect_files();
      refresh_shellui();
      notify("FPKG Guard: rebuilt %d title(s)", n);
      send_redirect(cli, "/");
    } else if(strstr(req, "GET /mlist")) {
      char *list = fpkg_db_manual_list();
      send_json(cli, list ? list : "[]");
      free(list);
    } else if(strstr(req, "GET /msnap")) {
      fpkg_db_manual_snapshot();
      notify("FPKG Guard: manual backup saved");
      send_redirect(cli, "/");
    } else if(strstr(req, "GET /snap")) {
      fpkg_db_full_snapshot();
      notify("FPKG Guard: snapshot saved");
      send_redirect(cli, "/");
    } else if(strstr(req, "GET /restore")) {
      const char *slot = strstr(req, "from=");
      char slotbuf[32] = "current";
      if(slot) {
        slot += 5;
        int i = 0;
        while(i < 31 && slot[i] && slot[i] != ' ' && slot[i] != '&' && slot[i] != '\r') {
          slotbuf[i] = slot[i];
          i++;
        }
        slotbuf[i] = '\0';
      }
      if(fpkg_db_full_restore(slotbuf) == 0) {
        refresh_shellui();
        notify("FPKG Guard: restored from '%s'", slotbuf);
      }
      send_redirect(cli, "/");
    } else
      send_page(cli);
    close(cli);
  }
  return 0;
}
