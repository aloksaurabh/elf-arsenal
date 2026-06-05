#include <stdlib.h>
#include <string.h>

#include "miniz.h"
#include "zipread.h"


struct ezip {
  mz_zip_archive zip;
  mz_uint        num;
  mz_uint        cursor;
  int            cur_valid;
  mz_zip_archive_file_stat cur_stat;
  char           err[160];
};

struct ezip_stream {
  mz_zip_reader_extract_iter_state *iter;
};


static void
ezip_seterr(ezip *z, const char *prefix) {
  if(!z) return;
  mz_zip_error e = mz_zip_get_last_error(&z->zip);
  const char *msg = e == MZ_ZIP_NO_ERROR ? "ok" : mz_zip_get_error_string(e);
  snprintf(z->err, sizeof(z->err), "%s: %s", prefix, msg);
}


static ezip *
ezip_finalize_open(ezip *z) {
  z->num    = mz_zip_reader_get_num_files(&z->zip);
  z->cursor = 0;
  return z;
}


ezip *
ezip_open_mem(const void *data, size_t len) {
  ezip *z = calloc(1, sizeof(*z));
  if(!z) return NULL;
  if(!mz_zip_reader_init_mem(&z->zip, data, len, 0)) {
    ezip_seterr(z, "init_mem");
    /* Keep handle so caller can ezip_error() then ezip_close. */
    return z;
  }
  return ezip_finalize_open(z);
}


ezip *
ezip_open_file(const char *path) {
  ezip *z = calloc(1, sizeof(*z));
  if(!z) return NULL;
  if(!mz_zip_reader_init_file(&z->zip, path, 0)) {
    ezip_seterr(z, "init_file");
    return z;
  }
  return ezip_finalize_open(z);
}


const char *
ezip_error(const ezip *z) {
  return z ? z->err : "null handle";
}


int
ezip_failed(const ezip *z) {
  return !z || z->err[0] != 0;
}


int
ezip_next(ezip *z, ezip_entry *out) {
  if(!z) return -1;
  z->cur_valid = 0;
  if(z->cursor >= z->num) return 0;
  if(!mz_zip_reader_file_stat(&z->zip, z->cursor, &z->cur_stat)) {
    ezip_seterr(z, "file_stat");
    z->cursor++;
    return -1;
  }
  out->path   = z->cur_stat.m_filename;
  out->size   = (int64_t)z->cur_stat.m_uncomp_size;
  out->is_dir = z->cur_stat.m_is_directory ? 1 : 0;
  z->cur_valid = 1;
  z->cursor++;
  return 1;
}


void *
ezip_extract_heap(ezip *z, size_t *out_size) {
  if(!z || !z->cur_valid) return NULL;
  size_t sz = 0;
  void *buf = mz_zip_reader_extract_to_heap(&z->zip, z->cursor - 1, &sz, 0);
  if(!buf) {
    ezip_seterr(z, "extract_to_heap");
    return NULL;
  }
  if(out_size) *out_size = sz;
  return buf;
}


ezip_stream *
ezip_stream_open(ezip *z) {
  if(!z || !z->cur_valid) return NULL;
  mz_zip_reader_extract_iter_state *iter =
    mz_zip_reader_extract_iter_new(&z->zip, z->cursor - 1, 0);
  if(!iter) {
    ezip_seterr(z, "extract_iter_new");
    return NULL;
  }
  ezip_stream *s = calloc(1, sizeof(*s));
  if(!s) { mz_zip_reader_extract_iter_free(iter); return NULL; }
  s->iter = iter;
  return s;
}


ssize_t
ezip_stream_read(ezip_stream *s, void *buf, size_t len) {
  if(!s || !s->iter) return -1;
  return (ssize_t)mz_zip_reader_extract_iter_read(s->iter, buf, len);
}


void
ezip_stream_close(ezip_stream *s) {
  if(!s) return;
  if(s->iter) mz_zip_reader_extract_iter_free(s->iter);
  free(s);
}


void
ezip_close(ezip *z) {
  if(!z) return;
  mz_zip_reader_end(&z->zip);
  free(z);
}
