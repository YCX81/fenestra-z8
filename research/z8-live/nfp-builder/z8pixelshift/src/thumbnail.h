/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS8_THUMBNAIL_H
#define PS8_THUMBNAIL_H
#include "job.h"
#define PS8_THUMB_W 160u
#define PS8_THUMB_H 120u
#define PS8_THUMB_RGBA (PS8_THUMB_W*PS8_THUMB_H*4u)
#define PS8_THUMB_JPEG_MAX 131072u
/* Bounded baseline sequential 8-bit YCbCr, 160x120. Only a browser preview;
   malformed/unsupported thumbnails return an error and do not block merging. */
size_t ps8_jpeg_workspace_bytes(void);
int ps8_jpeg_decode(const uint8_t *,uint32_t,uint8_t *,void *,size_t);
int ps8_thumbnail_read(const psm_platform *,const char *,uint8_t *,uint32_t *,uint8_t *);
#endif
