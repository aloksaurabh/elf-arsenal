#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Thin libarchive-shaped wrapper around miniz. We only need ZIP reads
 * (no other formats), so this is ~100 lines instead of 4 MB of
 * libarchive + lzma + zstd + bz2. */

typedef struct ezip        ezip;
typedef struct ezip_stream ezip_stream;

typedef struct ezip_entry {
  const char *path;     /* valid until next ezip_next / ezip_close */
  int64_t     size;
  int         is_dir;
} ezip_entry;

ezip *ezip_open_mem(const void *data, size_t len);
ezip *ezip_open_file(const char *path);
const char *ezip_error(const ezip *z);
int   ezip_failed(const ezip *z);  /* 1 if open or last op set an error */

/* Returns 1 on success, 0 at end-of-archive, -1 on error. */
int   ezip_next(ezip *z, ezip_entry *out);

/* One-shot extract of current entry into a malloc()'d buffer. NULL on
 * error. Caller frees. */
void *ezip_extract_heap(ezip *z, size_t *out_size);

/* Streaming read of current entry. Required when the file is too big
 * to keep in memory (extract-to-file paths). */
ezip_stream *ezip_stream_open(ezip *z);
ssize_t      ezip_stream_read(ezip_stream *s, void *buf, size_t len);
void         ezip_stream_close(ezip_stream *s);

void  ezip_close(ezip *z);
