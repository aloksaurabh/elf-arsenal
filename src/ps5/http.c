/* Copyright (C) 2026 John Törnblom

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>


typedef struct http_ctx {
  int libnetMemId;
  int libsslCtxId;
  int libhttpCtxId;
  int tmplId;
  int connId;
  int reqId;
} http_ctx_t;


int sceNetInit();
int sceNetPoolCreate(const char*, int, int);
int sceNetPoolDestroy(int);

int sceSslInit(size_t);
int sceSslTerm(int);

int sceHttpInit(int, int, size_t);
int sceHttpTerm(int);

int sceHttpCreateTemplate(int, const char*, int, int);
int sceHttpsSetSslCallback(int, void*, void*);
int sceHttpSetResponseHeaderMaxSize(int, size_t);
int sceHttpDeleteTemplate(int);

int sceHttpCreateConnectionWithURL(int, const char*, int);
int sceHttpDeleteConnection(int);

int sceHttpCreateRequestWithURL(int, int, const char*, uint64_t);
int sceHttpSendRequest(int, const void*, size_t);
int sceHttpGetResponseContentLength(int, int*, uint64_t*);
int sceHttpGetStatusCode(int, int*);
int sceHttpReadData(int, void *, size_t);
int sceHttpDeleteRequest(int);


static int
http_ssl_cb(void) {
  return 0;
}


static int
http_init(http_ctx_t* ctx, const char* agent, const char* method,
	  const char* url) {
  int err;

  ctx->libnetMemId  = -1;
  ctx->libsslCtxId  = -1;
  ctx->libhttpCtxId = -1;
  ctx->tmplId       = -1;
  ctx->reqId        = -1;

  if((err=sceNetInit()) < 0) {
    return err;
  }

  if((ctx->libnetMemId=sceNetPoolCreate("http_get", 16*1024, 0)) < 0) {
    return ctx->libnetMemId;
  }

  if((ctx->libsslCtxId=sceSslInit(128*1024)) < 0) {
    return ctx->libsslCtxId;
  }

  if((ctx->libhttpCtxId=sceHttpInit(ctx->libnetMemId, ctx->libsslCtxId,
				    32*1024)) < 0) {
    return ctx->libhttpCtxId;
  }

  if((ctx->tmplId=sceHttpCreateTemplate(ctx->libhttpCtxId, agent, 2, 1)) < 0) {
    return ctx->tmplId;
  }

  if((err=sceHttpSetResponseHeaderMaxSize(ctx->tmplId, 0x2000)) < 0) {
    return err;
  }

  if((err=sceHttpsSetSslCallback(ctx->tmplId, http_ssl_cb, 0))) {
    return err;
  }

  if((ctx->connId=sceHttpCreateConnectionWithURL(ctx->tmplId, url, 0)) < 0) {
    return ctx->connId;
  }

  if((ctx->reqId=sceHttpCreateRequestWithURL(ctx->connId, 0, url, 0)) < 0) {
    return ctx->reqId;
  }

  return 0;
}


static int
http_request(http_ctx_t* ctx, uint8_t** data, size_t* len) {
  int status = -1;
  int err;

  *data = NULL;
  *len  = 0;

  if((err=sceHttpSendRequest(ctx->reqId, 0, 0))) {
    return err;
  }
  if((err=sceHttpGetStatusCode(ctx->reqId, &status))) {
    return err;
  }

  if(status != 200) {
    return status;
  }

  int    cl_known = 1;
  size_t probed   = 0;
  if(sceHttpGetResponseContentLength(ctx->reqId, &cl_known, &probed) != 0) {
    cl_known = 1;
    probed   = 0;
  }

  size_t cap = (cl_known == 0 && probed > 0) ? probed : 16384;
  size_t off = 0;
  uint8_t *buf = malloc(cap);
  if(!buf) {
    return -1;
  }

  for(;;) {
    if(cap - off < 4096) {
      size_t new_cap = cap * 2;
      uint8_t *nb = realloc(buf, new_cap);
      if(!nb) { free(buf); return -1; }
      buf = nb;
      cap = new_cap;
    }
    int n = sceHttpReadData(ctx->reqId, buf + off, cap - off);
    if(n < 0) { free(buf); return n; }
    if(n == 0) break;
    off += (size_t)n;
  }

  *data = buf;
  *len  = off;
  return status;
}


static void
http_fini(http_ctx_t* ctx) {
  if(ctx->reqId >= 0) {
    sceHttpDeleteRequest(ctx->reqId);
  }
  if(ctx->connId >= 0) {
    sceHttpDeleteConnection(ctx->connId);
  }
  if(ctx->tmplId >= 0) {
    sceHttpDeleteTemplate(ctx->tmplId);
  }
  if(ctx->libhttpCtxId >= 0) {
    sceHttpTerm(ctx->libhttpCtxId);
  }
  if(ctx->libsslCtxId >= 0) {
    sceSslTerm(ctx->libsslCtxId);
  }
  if(ctx->libnetMemId >= 0) {
    sceNetPoolDestroy(ctx->libnetMemId);
  }
}


uint8_t*
http_get(const char* url, size_t* len) {
  http_ctx_t ctx;
  size_t size = 0;
  uint8_t* data;

  if((http_init(&ctx, "websrv/"VERSION_TAG, "GET", url))) {
    http_fini(&ctx);
    return 0;
  }

  if(http_request(&ctx, &data, &size) != 200) {
    http_fini(&ctx);
    return 0;
  }

  http_fini(&ctx);
  if(len) {
    *len = size;
  }
  return data;
}
