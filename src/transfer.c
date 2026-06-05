/* Elf Arsenal — file-manager copy/move/delete primitives + job tracker. */

#include <aio.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <microhttpd.h>

#include "third_party/cJSON.h"
#include "transfer.h"
#include "websrv.h"


#define COPY_SMALL_BUF  (16 * 1024)
#define COPY_LARGE_BUF  (1 * 1024 * 1024)
#define COPY_LARGE_THR  (1 * 1024 * 1024)


/* ───── Small / large async copy primitives (port from ps5-app-dumper) ───── */

static int
fs_nread(int fd, void *buf, size_t n) {
  uint8_t *p = buf;
  while(n > 0) {
    ssize_t r = read(fd, p, n);
    if(r < 0) { if(errno == EINTR) continue; return -1; }
    if(r == 0) { errno = EIO; return -1; }
    p += r;
    n -= (size_t)r;
  }
  return 0;
}

static int
fs_nwrite(int fd, const void *buf, size_t n) {
  const uint8_t *p = buf;
  while(n > 0) {
    ssize_t w = write(fd, p, n);
    if(w < 0) { if(errno == EINTR) continue; return -1; }
    if(w == 0) { errno = EIO; return -1; }
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

/* Job state — one copy/move/delete batch at a time. The UI keeps the
   user from queueing a second job. */
struct job_state {
  pthread_mutex_t lock;
  atomic_int      busy;
  atomic_int      cancel;
  atomic_long     total_bytes;
  atomic_long     copied_bytes;
  atomic_int      total_files;
  atomic_int      done_files;
  atomic_int      failed_files;
  char            current[512];
  char            verb[16];   /* "copy" | "move" | "delete" | "mkdir" | "rename" */
  char            error[256];
  time_t          started_at;
  time_t          ended_at;
};

static struct job_state g_job = {
  .lock = PTHREAD_MUTEX_INITIALIZER,
};


static void
job_set_current(const char *path) {
  pthread_mutex_lock(&g_job.lock);
  strncpy(g_job.current, path ? path : "", sizeof(g_job.current) - 1);
  g_job.current[sizeof(g_job.current) - 1] = 0;
  pthread_mutex_unlock(&g_job.lock);
}


static int
job_cancelled(void) { return atomic_load(&g_job.cancel); }


static int
fs_ncopy_small(int fd_in, int fd_out, size_t size) {
  char buf[COPY_SMALL_BUF];
  size_t copied = 0;
  while(copied < size) {
    if(job_cancelled()) { errno = ECANCELED; return -1; }
    size_t n = size - copied;
    if(n > sizeof(buf)) n = sizeof(buf);
    if(fs_nread(fd_in, buf, n)  != 0) return -1;
    if(fs_nwrite(fd_out, buf, n) != 0) return -1;
    copied += n;
    atomic_fetch_add(&g_job.copied_bytes, (long)n);
  }
  return 0;
}


static int
fs_ncopy_large(int src, int dst, size_t size) {
  void *buf = malloc(COPY_LARGE_BUF);
  if(!buf) return -1;

  struct aiocb aior = { .aio_fildes = src, .aio_buf = buf,
                        .aio_nbytes = COPY_LARGE_BUF, .aio_offset = 0 };
  struct aiocb aiow = { .aio_fildes = dst, .aio_buf = buf,
                        .aio_nbytes = COPY_LARGE_BUF, .aio_offset = 0 };
  size_t copied = 0;

  while(copied < size) {
    if(job_cancelled()) { free(buf); errno = ECANCELED; return -1; }
    if(copied + aior.aio_nbytes > size) aior.aio_nbytes = size - copied;
    if(aio_read(&aior) < 0) { free(buf); return -1; }
    aio_suspend(&(const struct aiocb*){&aior}, 1, NULL);
    ssize_t n = aio_return(&aior);
    if(n < 0 || (size_t)n != aior.aio_nbytes) { free(buf); return -1; }

    aiow.aio_buf    = aior.aio_buf;
    aiow.aio_nbytes = (size_t)n;
    if(aio_write(&aiow) < 0) { free(buf); return -1; }
    aio_suspend(&(const struct aiocb*){&aiow}, 1, NULL);
    if(aio_return(&aiow) < 0) { free(buf); return -1; }

    aior.aio_offset += n;
    aiow.aio_offset += n;
    copied += (size_t)n;
    atomic_fetch_add(&g_job.copied_bytes, (long)n);
  }
  free(buf);
  return 0;
}


/* Record the first per-file failure into the job_state error string so
   the UI shows the real reason instead of just "copy: success". */
static void
job_set_error_locked(const char *fmt, ...) {
  pthread_mutex_lock(&g_job.lock);
  if(!g_job.error[0]) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_job.error, sizeof(g_job.error), fmt, ap);
    va_end(ap);
  }
  pthread_mutex_unlock(&g_job.lock);
}


static int
copy_one_file(const char *src, const char *dst, mode_t mode) {
  int rc = -1;
  int sfd = open(src, O_RDONLY);
  if(sfd < 0) {
    job_set_error_locked("open(%s): %s", src, strerror(errno));
    return -1;
  }

  int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0777);
  if(dfd < 0) {
    job_set_error_locked("open(%s, write): %s", dst, strerror(errno));
    close(sfd);
    return -1;
  }

  struct stat st;
  if(fstat(sfd, &st) != 0) {
    job_set_error_locked("fstat(%s): %s", src, strerror(errno));
    close(sfd); close(dfd); unlink(dst);
    return -1;
  }
  size_t size = (size_t)st.st_size;

  job_set_current(src);
  if(size < COPY_LARGE_THR) {
    rc = fs_ncopy_small(sfd, dfd, size);
  } else {
    rc = fs_ncopy_large(sfd, dfd, size);
  }
  if(rc != 0) {
    job_set_error_locked("write(%s → %s): %s", src, dst, strerror(errno));
  }
  close(sfd);
  close(dfd);
  if(rc != 0) unlink(dst);
  return rc;
}


/* Pre-walk a directory tree to count files + total bytes for the
   progress bar. Called before the actual copy. */
static void
size_walker(const char *path, long *files, long *bytes) {
  struct stat st;
  if(lstat(path, &st) != 0) return;
  if(S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path);
    if(!d) return;
    struct dirent *ent;
    while((ent = readdir(d))) {
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      char child[1024];
      snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
      size_walker(child, files, bytes);
    }
    closedir(d);
  } else if(S_ISREG(st.st_mode)) {
    (*files)++;
    *bytes += st.st_size;
  }
}


static int
mkdirs(const char *path) {
  char buf[1024];
  size_t n = strlen(path);
  if(n >= sizeof(buf)) { errno = ENAMETOOLONG; return -1; }
  memcpy(buf, path, n + 1);
  /* Walk and mkdir along the way. */
  for(size_t i = 1; i <= n; i++) {
    if(buf[i] == '/' || buf[i] == 0) {
      char saved = buf[i];
      buf[i] = 0;
      if(mkdir(buf, 0777) != 0 && errno != EEXIST) return -1;
      buf[i] = saved;
    }
  }
  return 0;
}


static int
copy_recursive(const char *src, const char *dst) {
  if(job_cancelled()) { errno = ECANCELED; return -1; }
  struct stat st;
  if(lstat(src, &st) != 0) return -1;

  if(S_ISDIR(st.st_mode)) {
    if(mkdir(dst, 0777) != 0 && errno != EEXIST) return -1;
    DIR *d = opendir(src);
    if(!d) return -1;
    struct dirent *ent;
    int rc = 0;
    while((ent = readdir(d))) {
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      char s2[1024], d2[1024];
      snprintf(s2, sizeof(s2), "%s/%s", src, ent->d_name);
      snprintf(d2, sizeof(d2), "%s/%s", dst, ent->d_name);
      if(copy_recursive(s2, d2) != 0) {
        rc = -1;
        atomic_fetch_add(&g_job.failed_files, 1);
        if(job_cancelled()) break;
      }
    }
    closedir(d);
    return rc;
  }
  if(S_ISREG(st.st_mode)) {
    int rc = copy_one_file(src, dst, st.st_mode);
    if(rc == 0) atomic_fetch_add(&g_job.done_files, 1);
    return rc;
  }
  /* Symlinks / specials we just skip — not relevant for save/PKG copies. */
  return 0;
}


static int
delete_recursive(const char *path) {
  if(job_cancelled()) { errno = ECANCELED; return -1; }
  struct stat st;
  if(lstat(path, &st) != 0) return -1;

  if(S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path);
    if(!d) return -1;
    struct dirent *ent;
    int rc = 0;
    while((ent = readdir(d))) {
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      char child[1024];
      snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
      if(delete_recursive(child) != 0) {
        rc = -1;
        atomic_fetch_add(&g_job.failed_files, 1);
        if(job_cancelled()) break;
      }
    }
    closedir(d);
    if(rc == 0) {
      job_set_current(path);
      if(rmdir(path) != 0) return -1;
      atomic_fetch_add(&g_job.done_files, 1);
    }
    return rc;
  }
  job_set_current(path);
  if(unlink(path) != 0) return -1;
  atomic_fetch_add(&g_job.done_files, 1);
  return 0;
}



static enum MHD_Result
serve_buffer(struct MHD_Connection *conn, unsigned int status,
             const char *mime, void *data, size_t size, int free_after) {
  enum MHD_Result ret = MHD_NO;
  struct MHD_Response *resp;
  enum MHD_ResponseMemoryMode mode = free_after ? MHD_RESPMEM_MUST_FREE
                                                : MHD_RESPMEM_PERSISTENT;
  if((resp=MHD_create_response_from_buffer(size, data, mode))) {
    if(mime) MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, mime);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-cache");
    ret = websrv_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
  } else if(free_after) {
    free(data);
  }
  return ret;
}


static enum MHD_Result
serve_json(struct MHD_Connection *conn, unsigned int status, cJSON *o) {
  char *txt = cJSON_PrintUnformatted(o);
  if(!txt) return serve_buffer(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                               "application/json",
                               "{\"error\":\"alloc\"}", 17, 0);
  return serve_buffer(conn, status, "application/json", txt, strlen(txt), 1);
}


static enum MHD_Result
serve_error(struct MHD_Connection *conn, unsigned int status, const char *msg) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "ok", 0);
  cJSON_AddStringToObject(o, "error", msg);
  enum MHD_Result ret = serve_json(conn, status, o);
  cJSON_Delete(o);
  return ret;
}


static int
path_is_safe(const char *p) {
  if(!p || !*p) return 0;
  if(p[0] != '/') return 0;
  if(strstr(p, "..")) return 0;
  return 1;
}


static int
job_begin(const char *verb) {
  int expected = 0;
  if(!atomic_compare_exchange_strong(&g_job.busy, &expected, 1)) return 0;
  pthread_mutex_lock(&g_job.lock);
  atomic_store(&g_job.cancel, 0);
  atomic_store(&g_job.total_bytes,  0);
  atomic_store(&g_job.copied_bytes, 0);
  atomic_store(&g_job.total_files,  0);
  atomic_store(&g_job.done_files,   0);
  atomic_store(&g_job.failed_files, 0);
  g_job.current[0] = 0;
  g_job.error[0]   = 0;
  strncpy(g_job.verb, verb, sizeof(g_job.verb) - 1);
  g_job.verb[sizeof(g_job.verb) - 1] = 0;
  g_job.started_at = time(NULL);
  g_job.ended_at   = 0;
  pthread_mutex_unlock(&g_job.lock);
  return 1;
}


static void
job_end(int rc, const char *err) {
  pthread_mutex_lock(&g_job.lock);
  g_job.ended_at = time(NULL);
  if(rc != 0 && err) {
    strncpy(g_job.error, err, sizeof(g_job.error) - 1);
    g_job.error[sizeof(g_job.error) - 1] = 0;
  }
  pthread_mutex_unlock(&g_job.lock);
  atomic_store(&g_job.busy, 0);
}


/* Worker arg for copy/move. The handler stack does not survive the
   pthread, so we malloc + free in the worker. */
struct copy_arg {
  char src[512];
  char dst[512];
  int  is_move;
};


static void*
copy_worker(void *arg) {
  struct copy_arg *a = arg;

  /* Pre-walk to publish total bytes/files. Errors here are non-fatal
     — we just won't have an accurate progress bar. */
  long files = 0, bytes = 0;
  size_walker(a->src, &files, &bytes);
  atomic_store(&g_job.total_files, (int)files);
  atomic_store(&g_job.total_bytes, bytes);

  struct stat dst_st, src_st;
  char final_dst[1024];
  strncpy(final_dst, a->dst, sizeof(final_dst) - 1);
  final_dst[sizeof(final_dst) - 1] = 0;
  if(lstat(a->src, &src_st) != 0) {
    job_end(-1, "source not found");
    free(a);
    return NULL;
  }
  if(stat(a->dst, &dst_st) == 0 && S_ISDIR(dst_st.st_mode)) {
    const char *base = strrchr(a->src, '/');
    base = base ? base + 1 : a->src;
    snprintf(final_dst, sizeof(final_dst), "%s/%s", a->dst, base);
  }

  /* Make sure the parent dir of final_dst exists. */
  char parent[1024];
  strncpy(parent, final_dst, sizeof(parent) - 1);
  parent[sizeof(parent) - 1] = 0;
  char *slash = strrchr(parent, '/');
  if(slash && slash != parent) {
    *slash = 0;
    mkdirs(parent);
  }

  if(a->is_move) {
    /* Try a fast rename first — works when src/dst are on the same fs. */
    job_set_current(a->src);
    if(rename(a->src, final_dst) == 0) {
      atomic_store(&g_job.done_files, (int)(files > 0 ? files : 1));
      atomic_store(&g_job.copied_bytes, bytes);
      job_end(0, NULL);
      free(a);
      return NULL;
    }
    if(errno != EXDEV) {
      char err[160];
      snprintf(err, sizeof(err), "rename: %s", strerror(errno));
      job_end(-1, err);
      free(a);
      return NULL;
    }
    /* Cross-device move: fall back to copy then delete. */
  }

  int rc = copy_recursive(a->src, final_dst);
  if(rc != 0) {
    char err[160];
    snprintf(err, sizeof(err), "copy: %s", strerror(errno));
    job_end(-1, err);
    free(a);
    return NULL;
  }
  if(a->is_move) {
    /* Remove the source after a successful copy. */
    if(delete_recursive(a->src) != 0) {
      char err[160];
      snprintf(err, sizeof(err), "post-move cleanup: %s", strerror(errno));
      job_end(-1, err);
      free(a);
      return NULL;
    }
  }
  job_end(0, NULL);
  free(a);
  return NULL;
}


struct delete_arg {
  char path[512];
};


static void*
delete_worker(void *arg) {
  struct delete_arg *a = arg;

  long files = 0, bytes = 0;
  size_walker(a->path, &files, &bytes);
  atomic_store(&g_job.total_files, (int)(files > 0 ? files + 1 : 1));
  atomic_store(&g_job.total_bytes, bytes);

  if(delete_recursive(a->path) != 0) {
    char err[160];
    snprintf(err, sizeof(err), "delete: %s", strerror(errno));
    job_end(-1, err);
    free(a);
    return NULL;
  }
  job_end(0, NULL);
  free(a);
  return NULL;
}



static enum MHD_Result
list_request(struct MHD_Connection *conn) {
  const char *path = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "path");
  const char *fast = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "fast");
  int fast_mode = fast && strcmp(fast, "0") != 0;
  if(!path || !path_is_safe(path)) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "missing or unsafe 'path'");
  }
  DIR *d = opendir(path);
  if(!d) {
    return serve_error(conn, MHD_HTTP_NOT_FOUND, strerror(errno));
  }
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "path", path);
  cJSON_AddBoolToObject(r,   "fast", fast_mode);
  cJSON *entries = cJSON_AddArrayToObject(r, "entries");
  struct dirent *ent;
  while((ent = readdir(d))) {
    if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;

    int is_dir = -1;       /* -1 = unknown, 0 = file, 1 = dir */
#ifdef DT_DIR
    if(ent->d_type == DT_DIR) is_dir = 1;
    else if(ent->d_type == DT_REG ||
            ent->d_type == DT_LNK ||
            ent->d_type == DT_FIFO ||
            ent->d_type == DT_SOCK ||
            ent->d_type == DT_CHR ||
            ent->d_type == DT_BLK) is_dir = 0;
#endif

    if(fast_mode) {
      /* If d_type wasn't usable (DT_UNKNOWN on some filesystems) we
         fall through to a single lstat — only paid where we have to. */
      if(is_dir < 0) {
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        if(lstat(full, &st) != 0) continue;
        is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
      }
      cJSON *e = cJSON_CreateObject();
      cJSON_AddStringToObject(e, "name", ent->d_name);
      cJSON_AddBoolToObject(e,   "dir",  is_dir == 1);
      cJSON_AddItemToArray(entries, e);
      continue;
    }

    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
    struct stat st;
    if(lstat(full, &st) != 0) continue;
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "name", ent->d_name);
    cJSON_AddBoolToObject(e,   "dir",  S_ISDIR(st.st_mode));
    cJSON_AddNumberToObject(e, "size", (double)st.st_size);
    cJSON_AddNumberToObject(e, "mtime", (double)st.st_mtime);
    cJSON_AddItemToArray(entries, e);
  }
  closedir(d);
  cJSON_AddBoolToObject(r, "ok", 1);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


static enum MHD_Result
stat_request(struct MHD_Connection *conn) {
  const char *path = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "path");
  if(!path || !path_is_safe(path)) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "missing or unsafe 'path'");
  }
  struct stat st;
  if(lstat(path, &st) != 0) {
    return serve_error(conn, MHD_HTTP_NOT_FOUND, strerror(errno));
  }
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r,   "ok",    1);
  cJSON_AddStringToObject(r, "path",  path);
  cJSON_AddBoolToObject(r,   "dir",   S_ISDIR(st.st_mode));
  cJSON_AddNumberToObject(r, "size",  (double)st.st_size);
  cJSON_AddNumberToObject(r, "mtime", (double)st.st_mtime);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


/* GET /api/fs/usb — probe /mnt/usb0..usb7 for writable mounts. */
static enum MHD_Result
usb_request(struct MHD_Connection *conn) {
  cJSON *r = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(r, "mounts");
  for(int i = 0; i < 8; i++) {
    char path[24];
    snprintf(path, sizeof(path), "/mnt/usb%d", i);
    struct stat st;
    if(stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    /* Try a write probe so we only surface mounts that are actually
       writable. Cheaper than statvfs(). */
    char probe[40];
    snprintf(probe, sizeof(probe), "%s/.sl_probe", path);
    int fd = open(probe, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    int writable = 0;
    if(fd >= 0) { writable = 1; close(fd); unlink(probe); }
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "path", path);
    cJSON_AddBoolToObject(e,   "writable", writable);
    cJSON_AddItemToArray(arr, e);
  }
  cJSON_AddBoolToObject(r, "ok", 1);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


static enum MHD_Result
spawn_copy_or_move(struct MHD_Connection *conn, int is_move) {
  const char *src = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "src");
  const char *dst = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "dst");
  if(!src || !dst || !path_is_safe(src) || !path_is_safe(dst)) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "bad src/dst");
  }
  if(!strcmp(src, dst)) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "src == dst");
  }
  if(!job_begin(is_move ? "move" : "copy")) {
    return serve_error(conn, MHD_HTTP_CONFLICT,
                       "another file-manager job is already running");
  }
  struct copy_arg *a = calloc(1, sizeof(*a));
  if(!a) { job_end(-1, "alloc"); return serve_error(conn, 500, "alloc"); }
  strncpy(a->src, src, sizeof(a->src) - 1);
  strncpy(a->dst, dst, sizeof(a->dst) - 1);
  a->is_move = is_move;

  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  pthread_create(&t, &at, copy_worker, a);
  pthread_attr_destroy(&at);

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r,   "ok",  1);
  cJSON_AddStringToObject(r, "verb", is_move ? "move" : "copy");
  cJSON_AddStringToObject(r, "src",  src);
  cJSON_AddStringToObject(r, "dst",  dst);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


static enum MHD_Result
delete_handler(struct MHD_Connection *conn) {
  const char *path = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "path");
  const char *recursive = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "recursive");
  if(!path || !path_is_safe(path)) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "bad path");
  }
  /* Refuse to delete obvious roots. */
  if(!strcmp(path, "/") ||
     !strcmp(path, "/data") ||
     !strcmp(path, "/system_data") ||
     !strcmp(path, "/user")) {
    return serve_error(conn, MHD_HTTP_FORBIDDEN, "refusing to delete root path");
  }
  struct stat st;
  if(lstat(path, &st) != 0) {
    return serve_error(conn, MHD_HTTP_NOT_FOUND, strerror(errno));
  }
  if(S_ISDIR(st.st_mode) && (!recursive || !strcmp(recursive, "0"))) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                       "directory delete needs recursive=1");
  }
  if(!job_begin("delete")) {
    return serve_error(conn, MHD_HTTP_CONFLICT,
                       "another file-manager job is already running");
  }
  struct delete_arg *a = calloc(1, sizeof(*a));
  if(!a) { job_end(-1, "alloc"); return serve_error(conn, 500, "alloc"); }
  strncpy(a->path, path, sizeof(a->path) - 1);

  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  pthread_create(&t, &at, delete_worker, a);
  pthread_attr_destroy(&at);

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", 1);
  cJSON_AddStringToObject(r, "verb", "delete");
  cJSON_AddStringToObject(r, "path", path);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


static enum MHD_Result
mkdir_handler(struct MHD_Connection *conn) {
  const char *path = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "path");
  if(!path || !path_is_safe(path)) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "bad path");
  }
  if(mkdirs(path) != 0) {
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, strerror(errno));
  }
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", 1);
  cJSON_AddStringToObject(r, "path", path);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


static enum MHD_Result
rename_handler(struct MHD_Connection *conn) {
  const char *src = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "src");
  const char *dst = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "dst");
  if(!src || !dst || !path_is_safe(src) || !path_is_safe(dst)) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "bad src/dst");
  }
  if(rename(src, dst) != 0) {
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, strerror(errno));
  }
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", 1);
  cJSON_AddStringToObject(r, "src", src);
  cJSON_AddStringToObject(r, "dst", dst);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


static enum MHD_Result
status_handler(struct MHD_Connection *conn) {
  cJSON *r = cJSON_CreateObject();
  pthread_mutex_lock(&g_job.lock);
  int    busy   = atomic_load(&g_job.busy);
  long   tb     = atomic_load(&g_job.total_bytes);
  long   cb     = atomic_load(&g_job.copied_bytes);
  int    tf     = atomic_load(&g_job.total_files);
  int    df     = atomic_load(&g_job.done_files);
  int    ff     = atomic_load(&g_job.failed_files);
  int    cancel = atomic_load(&g_job.cancel);
  char   verb[16], current[512], err[256];
  strncpy(verb,    g_job.verb,    sizeof(verb));
  strncpy(current, g_job.current, sizeof(current));
  strncpy(err,     g_job.error,   sizeof(err));
  time_t started_at = g_job.started_at;
  time_t ended_at   = g_job.ended_at;
  pthread_mutex_unlock(&g_job.lock);

  cJSON_AddBoolToObject(r,   "ok",          1);
  cJSON_AddBoolToObject(r,   "busy",        busy ? 1 : 0);
  cJSON_AddBoolToObject(r,   "cancelling",  cancel ? 1 : 0);
  cJSON_AddStringToObject(r, "verb",        verb);
  cJSON_AddStringToObject(r, "current",     current);
  cJSON_AddStringToObject(r, "error",       err);
  cJSON_AddNumberToObject(r, "totalBytes",  (double)tb);
  cJSON_AddNumberToObject(r, "copiedBytes", (double)cb);
  cJSON_AddNumberToObject(r, "totalFiles",  tf);
  cJSON_AddNumberToObject(r, "doneFiles",   df);
  cJSON_AddNumberToObject(r, "failedFiles", ff);
  cJSON_AddNumberToObject(r, "startedAt",   (double)started_at);
  cJSON_AddNumberToObject(r, "endedAt",     (double)ended_at);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


static enum MHD_Result
cancel_handler(struct MHD_Connection *conn) {
  atomic_store(&g_job.cancel, 1);
  return status_handler(conn);
}



enum MHD_Result
transfer_request(struct MHD_Connection *conn, const char *url) {
  if(!strcmp(url, "/api/fs/list"))   return list_request(conn);
  if(!strcmp(url, "/api/fs/stat"))   return stat_request(conn);
  if(!strcmp(url, "/api/fs/usb"))    return usb_request(conn);
  if(!strcmp(url, "/api/fs/copy"))   return spawn_copy_or_move(conn, 0);
  if(!strcmp(url, "/api/fs/move"))   return spawn_copy_or_move(conn, 1);
  if(!strcmp(url, "/api/fs/delete")) return delete_handler(conn);
  if(!strcmp(url, "/api/fs/mkdir"))  return mkdir_handler(conn);
  if(!strcmp(url, "/api/fs/rename")) return rename_handler(conn);
  if(!strcmp(url, "/api/fs/job/status")) return status_handler(conn);
  if(!strcmp(url, "/api/fs/job/cancel")) return cancel_handler(conn);
  return serve_error(conn, MHD_HTTP_NOT_FOUND, "no such endpoint");
}



typedef struct {
  int    fd;
  char   final_path[1024];
  size_t bytes;
  int    init_failed;
  char   init_error[200];
} fs_upload_t;


void
fs_upload_free(void *state) {
  fs_upload_t *u = state;
  if(!u) return;
  if(u->fd >= 0) { close(u->fd); u->fd = -1; }
  free(u);
}


/* Reject path components that escape the destination (..) or contain
   embedded / null bytes. Each segment must also be non-empty. */
static int
fs_upload_segment_safe(const char *seg) {
  if(!seg || !*seg) return 0;
  if(!strcmp(seg, ".") || !strcmp(seg, "..")) return 0;
  for(const char *p = seg; *p; p++) {
    if(*p == '/' || *p == '\\') return 0;
  }
  return 1;
}


/* mkdir -p — recreate the dest dir tree before opening the file. */
static int
fs_upload_mkdir_p(const char *path) {
  if(!path || !*path) return 0;
  char buf[1024];
  size_t n = strlen(path);
  if(n >= sizeof(buf)) return -1;
  memcpy(buf, path, n + 1);
  for(size_t i = 1; i < n; i++) {
    if(buf[i] == '/') {
      buf[i] = '\0';
      mkdir(buf, 0777);
      buf[i] = '/';
    }
  }
  mkdir(buf, 0777);
  return 0;
}


enum MHD_Result
fs_upload_request(struct MHD_Connection *conn,
                  const char *upload_data,
                  size_t *upload_data_size,
                  void **state) {
  fs_upload_t *u = *state;

  if(!u) {
    u = calloc(1, sizeof(*u));
    if(!u) return MHD_NO;
    u->fd = -1;
    *state = u;

    const char *dest    = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "path");
    const char *fname   = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "filename");
    const char *relpath = MHD_lookup_connection_value(conn,
                              MHD_GET_ARGUMENT_KIND, "relpath");

    if(!dest || dest[0] != '/' || strstr(dest, "..")) {
      u->init_failed = 1;
      strncpy(u->init_error,
              "missing or unsafe ?path= (must be absolute, no '..')",
              sizeof(u->init_error) - 1);
      return MHD_YES;
    }
    if(!fname || !fs_upload_segment_safe(fname)) {
      u->init_failed = 1;
      strncpy(u->init_error,
              "missing or unsafe ?filename= (must be a single safe basename)",
              sizeof(u->init_error) - 1);
      return MHD_YES;
    }

    /* Build the destination directory: <dest>[/<relpath>]. relpath is
       trusted only after segment-by-segment validation. */
    char dir[1024];
    if((size_t)snprintf(dir, sizeof(dir), "%s", dest) >= sizeof(dir)) {
      u->init_failed = 1;
      strncpy(u->init_error, "destination path too long",
              sizeof(u->init_error) - 1);
      return MHD_YES;
    }
    /* Strip trailing slash. */
    size_t dlen = strlen(dir);
    while(dlen > 1 && dir[dlen - 1] == '/') { dir[--dlen] = '\0'; }

    if(relpath && *relpath) {
      char tmp[1024];
      if((size_t)snprintf(tmp, sizeof(tmp), "%s", relpath) >= sizeof(tmp)) {
        u->init_failed = 1;
        strncpy(u->init_error, "relpath too long",
                sizeof(u->init_error) - 1);
        return MHD_YES;
      }
      char *seg = tmp;
      while(seg && *seg) {
        char *slash = strchr(seg, '/');
        if(slash) *slash = '\0';
        if(*seg) {
          if(!fs_upload_segment_safe(seg)) {
            u->init_failed = 1;
            strncpy(u->init_error,
                    "unsafe relpath segment (must contain no .. or /)",
                    sizeof(u->init_error) - 1);
            return MHD_YES;
          }
          if((size_t)snprintf(dir + dlen, sizeof(dir) - dlen, "/%s", seg)
             >= sizeof(dir) - dlen) {
            u->init_failed = 1;
            strncpy(u->init_error, "relpath join too long",
                    sizeof(u->init_error) - 1);
            return MHD_YES;
          }
          dlen = strlen(dir);
        }
        if(!slash) break;
        seg = slash + 1;
      }
    }

    /* mkdir -p the destination. */
    fs_upload_mkdir_p(dir);

    if((size_t)snprintf(u->final_path, sizeof(u->final_path),
                        "%s/%s", dir, fname) >= sizeof(u->final_path)) {
      u->init_failed = 1;
      strncpy(u->init_error, "final path too long",
              sizeof(u->init_error) - 1);
      return MHD_YES;
    }
    u->fd = open(u->final_path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if(u->fd < 0) {
      u->init_failed = 1;
      snprintf(u->init_error, sizeof(u->init_error),
               "open %s: %s", u->final_path, strerror(errno));
      return MHD_YES;
    }
    return MHD_YES;
  }

  /* Streaming body chunks. */
  if(*upload_data_size > 0) {
    if(!u->init_failed && u->fd >= 0) {
      size_t want = *upload_data_size, off = 0;
      while(off < want) {
        ssize_t w = write(u->fd, upload_data + off, want - off);
        if(w <= 0) {
          u->init_failed = 1;
          snprintf(u->init_error, sizeof(u->init_error),
                   "write: %s", strerror(errno));
          break;
        }
        off += (size_t)w;
      }
      u->bytes += off;
    }
    *upload_data_size = 0;
    return MHD_YES;
  }

  /* End-of-body. */
  if(u->fd >= 0) { close(u->fd); u->fd = -1; }

  cJSON *r = cJSON_CreateObject();
  if(u->init_failed) {
    cJSON_AddBoolToObject(r,   "ok", 0);
    cJSON_AddStringToObject(r, "error", u->init_error);
    enum MHD_Result ret = serve_json(conn, MHD_HTTP_BAD_REQUEST, r);
    cJSON_Delete(r);
    fs_upload_free(u);
    *state = NULL;
    return ret;
  }
  cJSON_AddBoolToObject(r,   "ok", 1);
  cJSON_AddStringToObject(r, "path", u->final_path);
  cJSON_AddNumberToObject(r, "size", (double)u->bytes);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  fs_upload_free(u);
  *state = NULL;
  return ret;
}
