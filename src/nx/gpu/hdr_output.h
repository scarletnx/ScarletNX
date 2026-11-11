/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game's own HDR output (the Ounce's): what the frames of a window hold, and the metadata the window carries with
   them, as the front end (gpu/nvn/nvn_queue.c) hands them to the renderer (renderer.h present_hdr). */
#ifndef SNX_HDR_OUTPUT_H
#define SNX_HDR_OUTPUT_H
#include "common/cpu/aarch64/cpu.h"

/* what a presented frame's pixels are */
enum {
    SNX_HDR_SPACE_SRGB,  /* the console's SDR display's: shown as they are */
    SNX_HDR_SPACE_PQ,    /* HDR10: SMPTE ST 2084 (PQ) codes in BT.2020's primaries */
    SNX_HDR_SPACE_SCRGB, /* scRGB: linear light in sRGB's primaries, 1.0 at SNX_HDR_SCRGB_UNIT nits */
    SNX_HDR_SPACES
};
#define SNX_HDR_SCRGB_UNIT 80 /* the luminance of scRGB's 1.0, in nits: the console's driver's, and Vulkan's */

/* A frame's space and its window's HDR metadata, in the order of nvnWindowSetHdrDisplayMetadata's and
   nvnWindowSetHdrContentMetadata's arguments: metadata[0-7] the x and y of the mastering display's red, green and
   blue primaries and of its white point, [8] and [9] its minimum and maximum luminance in nits, [10] and [11] the
   content's maximum and maximum frame-average light levels in nits */
typedef struct {
    u32 space;
    f32 metadata[12];
} SnxHdrFrame;

/* the paper white a game is told and the menu is drawn at where the hdr_paperwhite setting is 0: BT.2408's reference
   white, in nits */
#define SNX_HDR_PAPER_WHITE 203
static inline u32 snx_hdr_paper_white(int setting) { return setting > 0 ? (u32)setting : SNX_HDR_PAPER_WHITE; }
#endif
