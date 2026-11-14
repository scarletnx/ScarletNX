/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/host1x/nvdec_common.h, src/video_core/host1x/codec_types.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project */
/* NVDEC's method registers and the structures in guest memory its jobs point at, as nvdec.c reads them.

   Methods are word indices into a channel's registers (byte offset / 4: NVIDIA's class header names SetApplicationId
   at byte 0x200, method 0x80). Eden's NvdecRegisters keeps each method in a u64 slot of the same index. A register
   that names memory holds an IOVA >> 8 (Eden's Offset::Address) */
#ifndef SNX_NVDEC_REGS_H
#define SNX_NVDEC_REGS_H
#include "common/cpu/aarch64/cpu.h"

/* NvdecRegisters' methods (ASSERT_REG_POSITION in nvdec_common.h) */
enum {
    NVDEC_SET_CODEC_ID = 0x80, /* set_codec_id, NVIDIA's SetApplicationId: written before every job */
    NVDEC_EXECUTE = 0xC0,      /* execute: the job runs */
    NVDEC_PICTURE_INFO_OFFSET = 0x101,
    NVDEC_FRAME_BITSTREAM_OFFSET = 0x102,
    NVDEC_FRAME_STATS_OFFSET = 0x109,
    NVDEC_SURFACE_LUMA_OFFSETS = 0x10C,   /* 17 of them, by picture index */
    NVDEC_SURFACE_CHROMA_OFFSETS = 0x11D, /* 17 */
    NVDEC_VP9_PROB_TAB_BUFFER_OFFSET = 0x170
};
#define NVDEC_SURFACES 17u
#define NVDEC_OFFSET_SHIFT 8u /* an Offset register holds the IOVA >> 8 */
static inline u64 nvdec_offset_address(u64 reg) { return reg << NVDEC_OFFSET_SHIFT; }

/* NvdecCommon::VideoCodec, the value of set_codec_id */
enum { NVDEC_CODEC_NONE = 0, NVDEC_CODEC_H264 = 3, NVDEC_CODEC_VP8 = 5, NVDEC_CODEC_H265 = 7, NVDEC_CODEC_VP9 = 9 };

/* H264DecoderContext (nvdec_h264_pic_s), at picture_info_offset, by byte offset. Its H264ParameterSet starts at
   0x58: each field below is 0x58 plus its offset there */
#define H264_CONTEXT_SIZE 0x300u /* 0x2FC, read as whole words */
enum {
    H264_STREAM_LEN = 0x48,
    H264_LOG2_MAX_PIC_ORDER_CNT_LSB_MINUS4 = 0x58,
    H264_DELTA_PIC_ORDER_ALWAYS_ZERO_FLAG = 0x5C,
    H264_FRAME_MBS_ONLY_FLAG = 0x60,
    H264_PIC_WIDTH_IN_MBS = 0x64,
    H264_FRAME_HEIGHT_IN_MBS = 0x68,
    H264_SURFACE_FORMAT = 0x6C, /* tile_format bits 0-1, gob_height bits 2-4 */
    H264_ENTROPY_CODING_MODE_FLAG = 0x70,
    H264_PIC_ORDER_PRESENT_FLAG = 0x74,
    H264_NUM_REFIDX_L0_DEFAULT_ACTIVE = 0x78,
    H264_NUM_REFIDX_L1_DEFAULT_ACTIVE = 0x7C,
    H264_DEBLOCKING_FILTER_CONTROL_PRESENT_FLAG = 0x80,
    H264_REDUNDANT_PIC_CNT_PRESENT_FLAG = 0x84,
    H264_TRANSFORM_8X8_MODE_FLAG = 0x88,
    H264_PITCH_LUMA = 0x8C,
    H264_PITCH_CHROMA = 0x90,
    H264_LUMA_FRAME_OFFSET = 0x9C,   /* Offset, added to the surface's */
    H264_CHROMA_FRAME_OFFSET = 0xA8, /* Offset */
    H264_FLAGS = 0xB0,               /* a u64: the bitfields below */
    H264_WEIGHT_SCALE_4X4 = 0x1C0,   /* 6 lists of 16, in raster order */
    H264_WEIGHT_SCALE_8X8 = 0x220,   /* 2 lists of 64 */
    H264_TRANSFORM_BYPASS = 0x2D0    /* lossless_ipred8x8_filter_enable bit 0, qpprime_y_zero_transform_bypass bit 1 */
};
#define H264_SCALING_LISTS_4X4 6u
#define H264_SCALING_LISTS_8X8 2u

/* surface format at 0x6C */
static inline u32 h264_tile_format(u32 v) { return v & 3u; } /* 0 block linear */
static inline u32 h264_gob_height(u32 v) { return (v >> 2) & 7u; }
/* qpprime_y_zero_transform_bypass_flag at 0x2D0 */
static inline u32 h264_qpprime_y_zero_transform_bypass(u32 v) { return (v >> 1) & 1u; }

/* the flags u64 at 0xB0, read as its low word lo and high word hi (H264ParameterSet's BitFields) */
static inline u32 h264_mbaff_frame(u32 lo) { return lo & 1u; }
static inline u32 h264_direct_8x8_inference(u32 lo) { return (lo >> 1) & 1u; }
static inline u32 h264_weighted_pred(u32 lo) { return (lo >> 2) & 1u; }
static inline u32 h264_constrained_intra_pred(u32 lo) { return (lo >> 3) & 1u; }
static inline u32 h264_field_pic(u32 lo) { return (lo >> 5) & 1u; }
static inline u32 h264_second_field(u32 lo) { return (lo >> 7) & 1u; }
static inline u32 h264_log2_max_frame_num_minus4(u32 lo) { return (lo >> 8) & 0xFu; }
static inline u32 h264_chroma_format_idc(u32 lo) { return (lo >> 12) & 3u; }
static inline u32 h264_pic_order_cnt_type(u32 lo) { return (lo >> 14) & 3u; }
/* the signed fields: their raw bits, sign extended by the caller */
static inline u32 h264_pic_init_qp_minus26_bits(u32 lo) { return (lo >> 16) & 0x3Fu; }
static inline u32 h264_chroma_qp_index_offset_bits(u32 lo) { return (lo >> 22) & 0x1Fu; }
static inline u32 h264_second_chroma_qp_index_offset_bits(u32 lo) { return lo >> 27; }
#define H264_PIC_INIT_QP_MINUS26_BITS 6u
#define H264_CHROMA_QP_INDEX_OFFSET_BITS 5u
static inline u32 h264_weighted_bipred_idc(u32 hi) { return hi & 3u; }
static inline u32 h264_curr_pic_idx(u32 hi) { return (hi >> 2) & 0x7Fu; } /* bits 34-40 of the u64 */

/* PictureInfo (VP9), at picture_info_offset, by byte offset */
#define VP9_PICTURE_INFO_SIZE 0x100u
enum {
    VP9_BITSTREAM_SIZE = 0x30,
    VP9_CURRENT_FRAME_WIDTH = 0x60, /* current_frame_size: u16 width, u16 height */
    VP9_CURRENT_FRAME_HEIGHT = 0x62
};
/* the probability table at vp9_prob_tab_buffer_offset (nvdec_vp9EntropyProbs_t) */
#define VP9_PROB_TABLE_SIZE 0xEA0u

/* VP8PictureInfo (nvdec_vp8_pic_s), at picture_info_offset, by byte offset */
#define VP8_PICTURE_INFO_SIZE 0xC0u
enum {
    VP8_FRAME_WIDTH = 0x38,       /* u16, the frame's own size */
    VP8_FRAME_HEIGHT = 0x3A,      /* u16 */
    VP8_KEY_FRAME = 0x3C,         /* u8 */
    VP8_VERSION = 0x3D,           /* u8 */
    VP8_SURFACE_FORMAT = 0x3E,    /* u8: tile_format bits 0-1, gob_height bits 2-4, as H.264's */
    VP8_FIRST_PART_SIZE = 0x40,   /* the first partition's bytes: the frame header and the modes */
    VP8_VLD_BUFFER_SIZE = 0x48,   /* the frame's bytes at frame_bitstream_offset */
    VP8_FRAME_STRIDE_LUMA = 0x4C, /* frame_stride[2], in VP8_FRAME_STRIDE_UNIT bytes */
    VP8_FRAME_STRIDE_CHROMA = 0x50,
    VP8_LUMA_FRAME_OFFSET = 0x5C,  /* Offset */
    VP8_CHROMA_FRAME_OFFSET = 0x68 /* Offset */
};
/* a 1280-wide movie's picture info gives frame strides of 80 */
#define VP8_FRAME_STRIDE_UNIT 16u
/* the surfaces a VP8 job names (Eden's Vp8SurfaceIndex): its references, then the picture it decodes */
enum { VP8_SURFACE_LAST, VP8_SURFACE_GOLDEN, VP8_SURFACE_ALTREF, VP8_SURFACE_CURRENT };

/* nvdec_status_s at frame_stats_offset: four u32, named as NVIDIA's Tegra headers name them */
#define NVDEC_STATUS_SIZE 16u
enum {
    NVDEC_STATUS_MBS_CORRECTLY_DECODED = 0x0,
    NVDEC_STATUS_MBS_IN_ERROR = 0x4,
    NVDEC_STATUS_CYCLE_COUNT = 0x8,
    NVDEC_STATUS_ERROR_STATUS = 0xC /* nonzero: the job failed */
};

#endif
