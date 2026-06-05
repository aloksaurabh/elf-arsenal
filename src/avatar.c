
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <microhttpd.h>

#include "avatar.h"
#include "sys.h"
#include "third_party/cJSON.h"
#include "third_party/stb_image.h"
#include "third_party/stb_image_write.h"
#include "websrv.h"


#define AVATAR_DIR    "/data/elf-arsenal/avatar"
#define AVATAR_IN_DIR "/data/elf-arsenal/avatar/in"
#define AVATAR_WORK   "/data/elf-arsenal/avatar/work"

/* The four sizes png_to_dds.py emits, in the same order. */
static const int AVATAR_SIZES[] = {64, 128, 260, 440};
#define AVATAR_NSIZES ((int)(sizeof(AVATAR_SIZES)/sizeof(AVATAR_SIZES[0])))

static enum MHD_Result apply_to_dir(struct MHD_Connection *conn,
                                    const char *dest,
                                    const char *user_label);
static int find_latest_source(char *path_out, size_t path_out_size);


/*  Tiny helpers                                                          */

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
serve_json(struct MHD_Connection *conn, unsigned int status, cJSON *obj) {
  char *txt = cJSON_PrintUnformatted(obj);
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
ensure_dir(const char *path) {
  struct stat st;
  if(stat(path, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
  return mkdir(path, 0755);
}

static void
ensure_avatar_dirs(void) {
  ensure_dir("/data");
  ensure_dir("/data/elf-arsenal");
  ensure_dir(AVATAR_DIR);
  ensure_dir(AVATAR_IN_DIR);
  ensure_dir(AVATAR_WORK);
  chmod(AVATAR_DIR,    0777);
  chmod(AVATAR_IN_DIR, 0777);
  chmod(AVATAR_WORK,   0777);
}


static int
is_safe_image_basename(const char *s) {
  if(!s || !*s) return 0;
  size_t n = strlen(s);
  if(n > 96) return 0;
  for(size_t i=0; i<n; i++) {
    char c = s[i];
    if(!isalnum((unsigned char)c) && c != '.' && c != '-' && c != '_' &&
       c != ' ') return 0;
  }
  if(strstr(s, "..")) return 0;
  return 1;
}


/*  Bilinear RGBA resize                                                  */

static uint8_t*
resize_rgba(const uint8_t *src, int sw, int sh, int dw, int dh) {
  uint8_t *dst = malloc((size_t)dw * dh * 4);
  if(!dst) return NULL;
  for(int y=0; y<dh; y++) {
    double ys = (double)y * sh / dh;
    int    y0 = (int)ys;
    int    y1 = y0 + 1; if(y1 >= sh) y1 = sh - 1;
    double yF = ys - y0;
    for(int x=0; x<dw; x++) {
      double xs = (double)x * sw / dw;
      int    x0 = (int)xs;
      int    x1 = x0 + 1; if(x1 >= sw) x1 = sw - 1;
      double xF = xs - x0;
      const uint8_t *p00 = &src[(y0*sw + x0)*4];
      const uint8_t *p10 = &src[(y0*sw + x1)*4];
      const uint8_t *p01 = &src[(y1*sw + x0)*4];
      const uint8_t *p11 = &src[(y1*sw + x1)*4];
      uint8_t *o = &dst[(y*dw + x)*4];
      for(int c=0; c<4; c++) {
        double v = p00[c]*(1.0-xF)*(1.0-yF) + p10[c]*xF*(1.0-yF) +
                   p01[c]*(1.0-xF)*yF       + p11[c]*xF*yF;
        if(v < 0) v = 0; if(v > 255) v = 255;
        o[c] = (uint8_t)(v + 0.5);
      }
    }
  }
  return dst;
}


/* Center-crop the source to a square. The largest centred square that
   fits in (sw,sh) becomes the (side,side) output. */
static uint8_t*
center_crop_square(const uint8_t *src, int sw, int sh, int *side_out) {
  int side = sw < sh ? sw : sh;
  int x0   = (sw - side) / 2;
  int y0   = (sh - side) / 2;
  uint8_t *dst = malloc((size_t)side * side * 4);
  if(!dst) return NULL;
  for(int y=0; y<side; y++) {
    memcpy(dst + (size_t)y * side * 4,
           src + ((y0 + y) * sw + x0) * 4,
           (size_t)side * 4);
  }
  *side_out = side;
  return dst;
}


/*  DXT5 / DDS encoder — port of png_to_dds.py compress_dxt5_block        */

static uint16_t rgb888_to_565(int r, int g, int b) {
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void rgb565_to_888(uint16_t c, int out[3]) {
  out[0] = (((c >> 11) & 0x1f) << 3);
  out[1] = (((c >>  5) & 0x3f) << 2);
  out[2] = (( c        & 0x1f) << 3);
}

static int color_distance(const int a[3], const int b[3]) {
  int dr = a[0]-b[0], dg = a[1]-b[1], db = a[2]-b[2];
  return dr*dr + dg*dg + db*db;
}


/* Compress one 4×4 RGBA block to 16 bytes of DXT5. */
static void
compress_dxt5_block(const uint8_t pixels[64], uint8_t out[16]) {
  int a_min = 255, a_max = 0;
  for(int i=0; i<16; i++) {
    int a = pixels[i*4 + 3];
    if(a < a_min) a_min = a;
    if(a > a_max) a_max = a;
  }
  uint8_t alpha0 = (uint8_t)a_max, alpha1 = (uint8_t)a_min;

  uint8_t apal[8];
  apal[0] = alpha0;
  apal[1] = alpha1;
  if(alpha0 > alpha1) {
    for(int i=0; i<6; i++)
      apal[2+i] = (uint8_t)(((6-i) * alpha0 + (1+i) * alpha1) / 7);
  } else {
    for(int i=0; i<4; i++)
      apal[2+i] = (uint8_t)(((4-i) * alpha0 + (1+i) * alpha1) / 5);
    apal[6] = 0; apal[7] = 255;
  }

  uint64_t aindex = 0;
  for(int i=0; i<16; i++) {
    int a = pixels[i*4 + 3];
    int best = 0, dist = 256;
    for(int j=0; j<8; j++) {
      int d = a - apal[j]; if(d < 0) d = -d;
      if(d < dist) { dist = d; best = j; }
    }
    aindex |= ((uint64_t)best) << (i*3);
  }
  out[0] = alpha0;
  out[1] = alpha1;
  for(int i=0; i<6; i++) out[2+i] = (uint8_t)((aindex >> (i*8)) & 0xff);

  int min_c[3] = {255,255,255}, max_c[3] = {0,0,0};
  for(int i=0; i<16; i++) {
    for(int c=0; c<3; c++) {
      int v = pixels[i*4 + c];
      if(v < min_c[c]) min_c[c] = v;
      if(v > max_c[c]) max_c[c] = v;
    }
  }
  uint16_t color0 = rgb888_to_565(max_c[0], max_c[1], max_c[2]);
  uint16_t color1 = rgb888_to_565(min_c[0], min_c[1], min_c[2]);
  if(color0 < color1) {
    uint16_t t = color0; color0 = color1; color1 = t;
    int tmp[3] = {min_c[0], min_c[1], min_c[2]};
    min_c[0] = max_c[0]; min_c[1] = max_c[1]; min_c[2] = max_c[2];
    max_c[0] = tmp[0]; max_c[1] = tmp[1]; max_c[2] = tmp[2];
  } else if(color0 == color1) {
    if(color0 < 0xffff) color0++;
  }

  int c0[3], c1[3];
  rgb565_to_888(color0, c0);
  rgb565_to_888(color1, c1);
  int pal[4][3];
  pal[0][0] = c0[0]; pal[0][1] = c0[1]; pal[0][2] = c0[2];
  pal[1][0] = c1[0]; pal[1][1] = c1[1]; pal[1][2] = c1[2];
  for(int c=0; c<3; c++) {
    pal[2][c] = (2*c0[c] + c1[c]) / 3;
    pal[3][c] = (c0[c] + 2*c1[c]) / 3;
  }

  uint32_t cindex = 0;
  for(int i=0; i<16; i++) {
    int p[3] = {pixels[i*4], pixels[i*4+1], pixels[i*4+2]};
    int best = 0;
    int dist = color_distance(p, pal[0]);
    for(int j=1; j<4; j++) {
      int d = color_distance(p, pal[j]);
      if(d < dist) { dist = d; best = j; }
    }
    cindex |= ((uint32_t)best) << (i*2);
  }
  out[8]  = color0 & 0xff; out[9]  = (color0 >> 8) & 0xff;
  out[10] = color1 & 0xff; out[11] = (color1 >> 8) & 0xff;
  out[12] = cindex & 0xff;
  out[13] = (cindex >> 8)  & 0xff;
  out[14] = (cindex >> 16) & 0xff;
  out[15] = (cindex >> 24) & 0xff;
}


static int
write_dxt5_dds(const uint8_t *rgba, int w, int h, const char *path) {
  /* Pad to multiples of 4 (avatar sizes already are, but safe). */
  int bw = (w + 3) / 4, bh = (h + 3) / 4;
  size_t comp_size = (size_t)bw * bh * 16;

  uint8_t *buf = malloc(128 + comp_size);
  if(!buf) return -1;
  memset(buf, 0, 128 + comp_size);

  /* DDS header — same field layout the python script writes. */
  memcpy(buf, "DDS ", 4);
  buf[4] = 124;                     /* dwSize */
  uint32_t flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000;
  buf[8]  = flags & 0xff;
  buf[9]  = (flags >> 8)  & 0xff;
  buf[10] = (flags >> 16) & 0xff;
  buf[11] = (flags >> 24) & 0xff;
  buf[12] = h & 0xff; buf[13] = (h >> 8) & 0xff;
  buf[14] = (h >> 16)&0xff; buf[15] = (h >> 24) & 0xff;
  buf[16] = w & 0xff; buf[17] = (w >> 8) & 0xff;
  buf[18] = (w >> 16)&0xff; buf[19] = (w >> 24) & 0xff;
  uint32_t lsz = (uint32_t)comp_size;
  buf[20] = lsz & 0xff; buf[21] = (lsz >> 8) & 0xff;
  buf[22] = (lsz >> 16)&0xff; buf[23] = (lsz >> 24) & 0xff;
  buf[76] = 32;                     /* pf size */
  buf[80] = 4;                      /* DDPF_FOURCC */
  memcpy(buf + 84, "DXT5", 4);
  buf[108] = 0; buf[109] = 0x10;    /* DDSCAPS_TEXTURE */

  uint8_t block[64];
  uint8_t *p = buf + 128;
  for(int by = 0; by < bh; by++) {
    for(int bx = 0; bx < bw; bx++) {
      for(int y = 0; y < 4; y++) {
        int py = by * 4 + y; if(py >= h) py = h - 1;
        for(int x = 0; x < 4; x++) {
          int px = bx * 4 + x; if(px >= w) px = w - 1;
          const uint8_t *src = &rgba[(py * w + px) * 4];
          uint8_t *dst = &block[(y * 4 + x) * 4];
          dst[0] = src[0]; dst[1] = src[1];
          dst[2] = src[2]; dst[3] = src[3];
        }
      }
      compress_dxt5_block(block, p);
      p += 16;
    }
  }

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if(fd < 0) { free(buf); return -1; }
  ssize_t n = write(fd, buf, 128 + comp_size);
  close(fd);
  free(buf);
  return (n == (ssize_t)(128 + comp_size)) ? 0 : -1;
}


/*  Conversion pipeline                                                   */

/* mode: "crop" (centre-crop to square, default) or "fit" (pad with
   transparent black to the longer side). */
static int
build_all_sizes(const uint8_t *rgba, int w, int h, const char *mode,
                char *err, size_t err_size) {
  ensure_avatar_dirs();

  /* Step 1: square the source. */
  uint8_t *square = NULL;
  int side = 0;

  if(!mode || !strcasecmp(mode, "crop")) {
    square = center_crop_square(rgba, w, h, &side);
  } else {
    /* Fit-with-padding: copy onto a transparent square the size of the
       longer side. */
    side = w > h ? w : h;
    square = calloc((size_t)side * side, 4);
    if(square) {
      int x0 = (side - w) / 2;
      int y0 = (side - h) / 2;
      for(int y = 0; y < h; y++) {
        memcpy(square + ((y0 + y) * side + x0) * 4,
               rgba   + (y * w) * 4,
               (size_t)w * 4);
      }
    }
  }
  if(!square) {
    snprintf(err, err_size, "could not square the source image");
    return -1;
  }

  /* Step 2: emit each target size as DDS + a PNG preview. */
  for(int i = 0; i < AVATAR_NSIZES; i++) {
    int sz = AVATAR_SIZES[i];
    uint8_t *resized = (sz == side)
                        ? square
                        : resize_rgba(square, side, side, sz, sz);
    if(!resized) {
      if(square) free(square);
      snprintf(err, err_size, "resize failed for %dx%d", sz, sz);
      return -1;
    }

    char dds_path[256], png_path[256];
    snprintf(dds_path, sizeof(dds_path), "%s/avatar%d.dds", AVATAR_WORK, sz);
    snprintf(png_path, sizeof(png_path), "%s/avatar%d.png", AVATAR_WORK, sz);

    if(write_dxt5_dds(resized, sz, sz, dds_path) != 0) {
      if(resized != square) free(resized);
      free(square);
      snprintf(err, err_size, "DDS write failed: %s", dds_path);
      return -1;
    }
    /* PNG preview using stb_image_write. */
    stbi_write_png(png_path, sz, sz, 4, resized, sz * 4);

    if(resized != square) free(resized);
  }
  free(square);
  return 0;
}


static int
read_all(const char *path, uint8_t **out, size_t *out_len) {
  struct stat st;
  if(stat(path, &st) != 0) return -1;
  if(st.st_size <= 0 || st.st_size > 64 * 1024 * 1024) return -1;
  int fd = open(path, O_RDONLY);
  if(fd < 0) return -1;
  uint8_t *buf = malloc(st.st_size);
  if(!buf) { close(fd); return -1; }
  ssize_t n = read(fd, buf, st.st_size);
  close(fd);
  if(n != st.st_size) { free(buf); return -1; }
  *out = buf;
  *out_len = (size_t)n;
  return 0;
}


/*  Streaming POST upload — same chunk pattern as the PKG installer       */

typedef struct {
  int    fd;
  char   path[256];
  size_t bytes;
  char   mode[16];     /* crop | fit */
  int    init_failed;
  char   init_error[160];
} avatar_upload_t;


void
avatar_upload_free(void *state) {
  avatar_upload_t *u = state;
  if(!u) return;
  if(u->fd >= 0) { close(u->fd); u->fd = -1; }
  free(u);
}


enum MHD_Result
avatar_upload_request(struct MHD_Connection *conn,
                      const char *upload_data,
                      size_t *upload_data_size,
                      void **state) {
  avatar_upload_t *u = *state;

  if(!u) {
    u = calloc(1, sizeof(*u));
    u->fd = -1;
    *state = u;

    const char *name = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "filename");
    const char *mode = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "mode");
    strncpy(u->mode, (mode && *mode) ? mode : "crop", sizeof(u->mode)-1);

    if(!name || !*name) {
      u->init_failed = 1;
      strncpy(u->init_error, "missing 'filename' query arg",
              sizeof(u->init_error)-1);
      return MHD_YES;
    }
    if(!is_safe_image_basename(name)) {
      u->init_failed = 1;
      snprintf(u->init_error, sizeof(u->init_error),
               "bad filename: %.80s", name);
      return MHD_YES;
    }

    ensure_avatar_dirs();
    if(snprintf(u->path, sizeof(u->path), "%s/%s", AVATAR_IN_DIR, name)
       >= (int)sizeof(u->path)) {
      u->init_failed = 1;
      strncpy(u->init_error, "filename too long", sizeof(u->init_error)-1);
      u->path[0] = 0;
      return MHD_YES;
    }

    u->fd = open(u->path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(u->fd < 0) {
      u->init_failed = 1;
      snprintf(u->init_error, sizeof(u->init_error),
               "open %s: %s", u->path, strerror(errno));
      u->path[0] = 0;
      return MHD_YES;
    }
    return MHD_YES;
  }

  if(*upload_data_size > 0) {
    if(!u->init_failed && u->fd >= 0) {
      ssize_t want = (ssize_t)*upload_data_size, off = 0;
      while(off < want) {
        ssize_t w = write(u->fd, upload_data + off, want - off);
        if(w <= 0) {
          u->init_failed = 1;
          snprintf(u->init_error, sizeof(u->init_error),
                   "write: %s", strerror(errno));
          break;
        }
        off += w;
      }
      u->bytes += (size_t)off;
    }
    *upload_data_size = 0;
    return MHD_YES;
  }

  /* End of body — close file, run conversion. */
  if(u->fd >= 0) { close(u->fd); u->fd = -1; }

  cJSON *r = cJSON_CreateObject();
  if(u->init_failed || u->bytes == 0) {
    cJSON_AddBoolToObject(r, "ok", 0);
    cJSON_AddStringToObject(r, "error",
                            u->init_failed ? u->init_error : "empty upload");
    enum MHD_Result ret = serve_json(conn, MHD_HTTP_BAD_REQUEST, r);
    cJSON_Delete(r);
    free(u);
    *state = NULL;
    return ret;
  }

  /* Decode + build all sizes. */
  uint8_t *img = NULL;
  size_t img_len = 0;
  if(read_all(u->path, &img, &img_len) != 0) {
    cJSON_AddBoolToObject(r, "ok", 0);
    cJSON_AddStringToObject(r, "error", "could not re-read upload");
    enum MHD_Result ret = serve_json(conn,
                                     MHD_HTTP_INTERNAL_SERVER_ERROR, r);
    cJSON_Delete(r);
    free(u);
    *state = NULL;
    return ret;
  }
  int w = 0, h = 0, c = 0;
  uint8_t *rgba = stbi_load_from_memory(img, (int)img_len, &w, &h, &c, 4);
  free(img);
  if(!rgba || w <= 0 || h <= 0) {
    cJSON_AddBoolToObject(r, "ok", 0);
    char err[160];
    snprintf(err, sizeof(err), "decode failed: %s", stbi_failure_reason());
    cJSON_AddStringToObject(r, "error", err);
    if(rgba) stbi_image_free(rgba);
    enum MHD_Result ret = serve_json(conn, MHD_HTTP_BAD_REQUEST, r);
    cJSON_Delete(r);
    free(u);
    *state = NULL;
    return ret;
  }

  char err[256] = {0};
  int rc = build_all_sizes(rgba, w, h, u->mode, err, sizeof(err));
  stbi_image_free(rgba);

  if(rc != 0) {
    cJSON_AddBoolToObject(r, "ok", 0);
    cJSON_AddStringToObject(r, "error", err[0] ? err : "build failed");
    enum MHD_Result ret = serve_json(conn,
                                     MHD_HTTP_INTERNAL_SERVER_ERROR, r);
    cJSON_Delete(r);
    free(u);
    *state = NULL;
    return ret;
  }

  cJSON_AddBoolToObject(r,   "ok", 1);
  cJSON_AddStringToObject(r, "source", u->path);
  cJSON_AddNumberToObject(r, "size",   (double)u->bytes);
  cJSON_AddNumberToObject(r, "sourceWidth",  w);
  cJSON_AddNumberToObject(r, "sourceHeight", h);
  cJSON_AddStringToObject(r, "mode",   u->mode);
  cJSON *sizes = cJSON_AddArrayToObject(r, "sizes");
  for(int i=0; i<AVATAR_NSIZES; i++) {
    cJSON_AddItemToArray(sizes, cJSON_CreateNumber(AVATAR_SIZES[i]));
  }
  cJSON_AddStringToObject(r, "workDir", AVATAR_WORK);

  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  free(u);
  *state = NULL;
  return ret;
}


/*  GET /api/avatar/preview?size=N — serve PNG preview                    */

static enum MHD_Result
preview_request(struct MHD_Connection *conn) {
  const char *size_s = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "size");
  if(!size_s) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "missing size");
  }
  int size = atoi(size_s);
  int ok = 0;
  for(int i=0; i<AVATAR_NSIZES; i++) {
    if(AVATAR_SIZES[i] == size) { ok = 1; break; }
  }
  if(!ok) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                       "size must be 64, 128, 260 or 440");
  }

  char path[256];
  snprintf(path, sizeof(path), "%s/avatar%d.png", AVATAR_WORK, size);
  uint8_t *body = NULL;
  size_t blen = 0;
  if(read_all(path, &body, &blen) != 0) {
    return serve_error(conn, MHD_HTTP_NOT_FOUND,
                       "no preview for that size — run /api/avatar/upload first");
  }
  return serve_buffer(conn, MHD_HTTP_OK, "image/png", body, blen, 1);
}


/*  GET /api/avatar/adjust?mode=crop|fit — re-build with new mode         */

static enum MHD_Result
adjust_request(struct MHD_Connection *conn) {
  const char *mode = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "mode");
  if(!mode) mode = "crop";
  if(strcasecmp(mode, "crop") != 0 && strcasecmp(mode, "fit") != 0) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                       "mode must be 'crop' or 'fit'");
  }

  /* Re-read the most recent upload (the one in /avatar/in/). */
  ensure_avatar_dirs();
  DIR *d = opendir(AVATAR_IN_DIR);
  if(!d) {
    return serve_error(conn, MHD_HTTP_NOT_FOUND, "no source image — upload one first");
  }
  char src[256] = {0};
  time_t newest = 0;
  struct dirent *ent;
  while((ent = readdir(d))) {
    if(ent->d_name[0] == '.') continue;
    char full[512];
    snprintf(full, sizeof(full), "%s/%s", AVATAR_IN_DIR, ent->d_name);
    struct stat st;
    if(stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
    if(st.st_mtime > newest) {
      newest = st.st_mtime;
      strncpy(src, full, sizeof(src)-1);
      src[sizeof(src)-1] = 0;
    }
  }
  closedir(d);
  if(!src[0]) {
    return serve_error(conn, MHD_HTTP_NOT_FOUND, "no source image");
  }

  uint8_t *img = NULL;
  size_t img_len = 0;
  if(read_all(src, &img, &img_len) != 0) {
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "read source");
  }
  int w = 0, h = 0, c = 0;
  uint8_t *rgba = stbi_load_from_memory(img, (int)img_len, &w, &h, &c, 4);
  free(img);
  if(!rgba) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST, "decode failed");
  }
  char err[256] = {0};
  int rc = build_all_sizes(rgba, w, h, mode, err, sizeof(err));
  stbi_image_free(rgba);
  if(rc != 0) {
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                       err[0] ? err : "build failed");
  }
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", 1);
  cJSON_AddStringToObject(r, "mode", mode);
  cJSON_AddStringToObject(r, "source", src);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


/*  GET /api/avatar/apply?dest=/path/to/avatar — copy DDS files           */

static int
copy_file(const char *src, const char *dst) {
  int sfd = open(src, O_RDONLY);
  if(sfd < 0) return -1;
  int dfd = open(dst, O_WRONLY|O_CREAT|O_TRUNC, 0644);
  if(dfd < 0) { close(sfd); return -1; }
  uint8_t buf[8192];
  ssize_t n;
  while((n = read(sfd, buf, sizeof(buf))) > 0) {
    if(write(dfd, buf, n) != n) { close(sfd); close(dfd); unlink(dst); return -1; }
  }
  close(sfd);
  close(dfd);
  return 0;
}


static enum MHD_Result
apply_request(struct MHD_Connection *conn) {
  const char *dest = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "dest");
  if(!dest || !*dest) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                       "missing 'dest' (e.g. /user/home/<userId>/avatar/)");
  }
  if(dest[0] != '/' || strstr(dest, "..")) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                       "dest must be an absolute path with no '..'");
  }
  return apply_to_dir(conn, dest, NULL);
}


static int
find_latest_source(char *path_out, size_t path_out_size) {
  ensure_avatar_dirs();
  DIR *d = opendir(AVATAR_IN_DIR);
  if(!d) return -1;
  time_t newest = 0;
  path_out[0] = 0;
  struct dirent *ent;
  while((ent = readdir(d))) {
    if(ent->d_name[0] == '.') continue;
    char full[512];
    snprintf(full, sizeof(full), "%s/%s", AVATAR_IN_DIR, ent->d_name);
    struct stat st;
    if(stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
    if(st.st_mtime > newest) {
      newest = st.st_mtime;
      strncpy(path_out, full, path_out_size - 1);
      path_out[path_out_size - 1] = 0;
    }
  }
  closedir(d);
  return path_out[0] ? 0 : -1;
}


static enum MHD_Result
apply_to_dir(struct MHD_Connection *conn,
             const char *dest, const char *user_label) {
  /* Make sure the destination directory exists. */
  if(mkdir(dest, 0755) != 0 && errno != EEXIST) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 0);
    char err[160];
    snprintf(err, sizeof(err), "mkdir %s: %s", dest, strerror(errno));
    cJSON_AddStringToObject(r, "error", err);
    enum MHD_Result ret = serve_json(conn,
                                     MHD_HTTP_INTERNAL_SERVER_ERROR, r);
    cJSON_Delete(r);
    return ret;
  }

  size_t dlen = strlen(dest);
  const char *sep = (dlen > 0 && dest[dlen-1] == '/') ? "" : "/";

  int copied = 0, failed = 0;
  cJSON *files = cJSON_CreateArray();

  static const char *const KIND_NAMES[] = { "avatar", "picture" };
  for(int k=0; k<2; k++) {
    const char *kind = KIND_NAMES[k];
    for(int i=0; i<AVATAR_NSIZES; i++) {
      int sz = AVATAR_SIZES[i];
      char src[256], dst[512];
      /* The encode pipeline only writes avatar*.dds in the work dir;
         picture*.dds reuses the same source bytes via copy_file. */
      snprintf(src, sizeof(src), "%s/avatar%d.dds", AVATAR_WORK, sz);
      snprintf(dst, sizeof(dst), "%s%s%s%d.dds", dest, sep, kind, sz);

      cJSON *e = cJSON_CreateObject();
      cJSON_AddStringToObject(e, "src", src);
      cJSON_AddStringToObject(e, "dst", dst);
      if(copy_file(src, dst) == 0) {
        cJSON_AddBoolToObject(e, "ok", 1);
        copied++;
      } else {
        cJSON_AddBoolToObject(e, "ok", 0);
        cJSON_AddStringToObject(e, "error", strerror(errno));
        failed++;
      }
      cJSON_AddItemToArray(files, e);
    }
  }

  /* And the source PNG copied twice: avatar.png + picture.png. */
  char src_path[512];
  int have_src = (find_latest_source(src_path, sizeof(src_path)) == 0);
  if(have_src) {
    for(int k=0; k<2; k++) {
      char dst_png[512];
      snprintf(dst_png, sizeof(dst_png),
               "%s%s%s.png", dest, sep, KIND_NAMES[k]);
      cJSON *e = cJSON_CreateObject();
      cJSON_AddStringToObject(e, "src", src_path);
      cJSON_AddStringToObject(e, "dst", dst_png);
      if(copy_file(src_path, dst_png) == 0) {
        cJSON_AddBoolToObject(e, "ok", 1);
        copied++;
      } else {
        cJSON_AddBoolToObject(e, "ok", 0);
        cJSON_AddStringToObject(e, "error", strerror(errno));
        failed++;
      }
      cJSON_AddItemToArray(files, e);
    }
  }

  {
    /* user_label is "Name (0x<hex>)" — pull just the bare name out. */
    char online_name[64] = "";
    if(user_label) {
      const char *paren = strrchr(user_label, '(');
      size_t namelen = paren ? (size_t)(paren - user_label) : strlen(user_label);
      while(namelen > 0 && user_label[namelen-1] == ' ') namelen--;
      if(namelen >= sizeof(online_name)) namelen = sizeof(online_name) - 1;
      memcpy(online_name, user_label, namelen);
      online_name[namelen] = 0;
    }

    cJSON *oj = cJSON_CreateObject();
    cJSON_AddStringToObject(oj, "avatarUrl",
        "http://static-resource.np.community.playstation.net/"
        "avatar_xl/WWS_E/E0012_XL.png");
    cJSON_AddStringToObject(oj, "firstName", online_name);
    cJSON_AddStringToObject(oj, "lastName",  "");
    cJSON_AddStringToObject(oj, "pictureUrl",
        "https://image.api.np.km.playstation.net/images/"
        "?format=png&w=440&h=440"
        "&image=https%3A%2F%2Fkfscdn.api.np.km.playstation.net"
        "%2F00000000000008%2F000000000000003.png"
        "&sign=blablabla019501");
    cJSON_AddStringToObject(oj, "trophySummary",
        "{\"level\":1,\"progress\":0,\"earnedTrophies\":"
        "{\"platinum\":0,\"gold\":0,\"silver\":0,\"bronze\":0}}");
    cJSON_AddStringToObject(oj, "isOfficiallyVerified", "true");

    char *body = cJSON_PrintUnformatted(oj);
    cJSON_Delete(oj);

    char online_path[512];
    snprintf(online_path, sizeof(online_path),
             "%s%sonline.json", dest, sep);
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "src", "<generated>");
    cJSON_AddStringToObject(e, "dst", online_path);
    if(body) {
      int fd = open(online_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
      int rc = -1;
      if(fd >= 0) {
        size_t blen = strlen(body), off = 0;
        while(off < blen) {
          ssize_t w = write(fd, body + off, blen - off);
          if(w <= 0) break;
          off += (size_t)w;
        }
        if(off == blen) rc = 0;
        close(fd);
      }
      free(body);
      if(rc == 0) {
        cJSON_AddBoolToObject(e, "ok", 1);
        copied++;
      } else {
        cJSON_AddBoolToObject(e, "ok", 0);
        cJSON_AddStringToObject(e, "error", strerror(errno));
        failed++;
      }
    } else {
      cJSON_AddBoolToObject(e, "ok", 0);
      cJSON_AddStringToObject(e, "error", "cJSON_PrintUnformatted failed");
      failed++;
    }
    cJSON_AddItemToArray(files, e);
  }

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r,   "ok",     failed == 0);
  cJSON_AddStringToObject(r, "dest",   dest);
  if(user_label) cJSON_AddStringToObject(r, "user", user_label);
  cJSON_AddNumberToObject(r, "copied", copied);
  cJSON_AddNumberToObject(r, "failed", failed);
  cJSON_AddItemToObject(r,   "files",  files);
  enum MHD_Result ret = serve_json(conn,
                                   failed == 0 ? MHD_HTTP_OK
                                               : MHD_HTTP_INTERNAL_SERVER_ERROR,
                                   r);
  cJSON_Delete(r);
  return ret;
}


/* Build the standard PSN profile-cache directory for a given userId. */
static void
build_profile_cache_dir(uint32_t uid, char *out, size_t out_size) {
  snprintf(out, out_size,
           "/system_data/priv/cache/profile/0x%08X", uid);
}


static enum MHD_Result
whoami_request(struct MHD_Connection *conn) {
  char name[24] = {0};
  uint32_t uid = sys_get_foreground_user(name, sizeof(name));

  cJSON *r = cJSON_CreateObject();
  cJSON_AddNumberToObject(r, "userId", (double)uid);
  if(uid) {
    char hex[16];
    snprintf(hex, sizeof(hex), "0x%08X", uid);
    cJSON_AddStringToObject(r, "userIdHex", hex);
    cJSON_AddStringToObject(r, "userName", name[0] ? name : "?");

    char dir[128];
    build_profile_cache_dir(uid, dir, sizeof(dir));
    cJSON_AddStringToObject(r, "profileDir", dir);
    cJSON_AddBoolToObject(r,  "ok", 1);
  } else {
    cJSON_AddBoolToObject(r,  "ok", 0);
    cJSON_AddStringToObject(r, "error",
        "no foreground user — sign in to a profile first");
  }
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


static enum MHD_Result
apply_current_user_request(struct MHD_Connection *conn) {
  char name[24] = {0};
  uint32_t uid = sys_get_foreground_user(name, sizeof(name));
  if(!uid) {
    return serve_error(conn, MHD_HTTP_PRECONDITION_FAILED,
        "no foreground user — sign in to a profile first");
  }

  char dir[128];
  build_profile_cache_dir(uid, dir, sizeof(dir));
  /* Make sure the parent /system_data/priv/cache/profile/ exists. */
  mkdir("/system_data/priv/cache",         0755);
  mkdir("/system_data/priv/cache/profile", 0755);

  char label[64];
  snprintf(label, sizeof(label), "%s (0x%08X)",
           name[0] ? name : "?", uid);
  return apply_to_dir(conn, dir, label);
}


/*  GET /api/avatar/info — output dirs + sizes                            */

static enum MHD_Result
info_request(struct MHD_Connection *conn) {
  ensure_avatar_dirs();
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "inputDir",  AVATAR_IN_DIR);
  cJSON_AddStringToObject(r, "workDir",   AVATAR_WORK);
  cJSON *sizes = cJSON_AddArrayToObject(r, "sizes");
  for(int i=0; i<AVATAR_NSIZES; i++) {
    cJSON_AddItemToArray(sizes, cJSON_CreateNumber(AVATAR_SIZES[i]));
  }
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


/*  Dispatcher                                                            */

enum MHD_Result
avatar_request(struct MHD_Connection *conn, const char *url) {
  if(!strcmp(url, "/api/avatar"))                return info_request(conn);
  if(!strcmp(url, "/api/avatar/info"))           return info_request(conn);
  if(!strcmp(url, "/api/avatar/preview"))        return preview_request(conn);
  if(!strcmp(url, "/api/avatar/adjust"))         return adjust_request(conn);
  if(!strcmp(url, "/api/avatar/apply"))          return apply_request(conn);
  if(!strcmp(url, "/api/avatar/whoami"))         return whoami_request(conn);
  if(!strcmp(url, "/api/avatar/apply-current-user"))
    return apply_current_user_request(conn);
  return serve_error(conn, MHD_HTTP_NOT_FOUND, "no such endpoint");
}
