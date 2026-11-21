/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NX_NN_TVMR_JPEG_H
#define SNX_NX_NN_TVMR_JPEG_H
#include "common/cpu/aarch64/cpu.h"
#include "common/platform/thread.h"

/* The native TVMR JPEG decoder and encoder of the movie library (tvmr_jpeg.c, tvmr_jpeg.hook), which drive NVJPG on
   the console: the decoders and encoders open at once, each an object of TVMRJPEG_OBJECT bytes of guest memory that
   the runtime gives (a page of the kernel region, taken at the first create), named by its address */
#define TVMRJPEG_OBJECTS 32
#define TVMRJPEG_OBJECT  0x40u
#define TVMRJPEG_BUFS    16 /* an encoder's frames in flight at most, as the SDK clamps them */

enum { TVMRJPEG_FREE, TVMRJPEG_DECODER, TVMRJPEG_ENCODER };

/* a decoder or an encoder (g_hle.tvmr_jpeg, which a save state holds) */
typedef struct {
    u8 kind; /* TVMRJPEG_*: 0 for a free slot */
    /* the decoder: the maximum picture and bitstream that Create or Resize gave, the color standard of Render's RGB
       (0 BT.601, 1 BT.709, else full range) and the alpha it writes */
    u16 max_w, max_h;
    u32 max_bytes, color_standard, alpha;
    /* the encoder: the frames it holds (nbufs at most), the next one to write and to read, the room of its output
       buffer (the size Create gave less the default header's 591 bytes), the restart interval and the target size
       that SetAttributes gave, and its quantization tables, luma then chroma, in natural order */
    u8 nbufs, inflight, wr, rd;
    u32 cap, restart, target;
    u8 quant[2][64];
    u32 size[TVMRJPEG_BUFS];   /* each frame's JPEG, SOI to EOI, in bytes */
    u32 serial[TVMRJPEG_BUFS]; /* the frame's number: its bytes are g_host's while that holds the same number */
} TvmrJpegObj;

typedef struct {
    u64 base;   /* the objects' page, 0 until the first create */
    u32 next;   /* the pool's search */
    u32 frames; /* the encoders' frames so far, which number them */
    TvmrJpegObj obj[TVMRJPEG_OBJECTS];
} HleTvmrJpeg;

/* the encoded frames, which a save state doesn't hold: a frame fed before a load and read after it is lost */
typedef struct {
    PMutex lk; /* the pool and each object */
    u8* jpeg[TVMRJPEG_OBJECTS][TVMRJPEG_BUFS];
    u32 serial[TVMRJPEG_OBJECTS][TVMRJPEG_BUFS];
} HostTvmrJpeg;
#endif
