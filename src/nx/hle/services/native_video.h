/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NATIVE_VIDEO_H
#define SNX_NATIVE_VIDEO_H
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/state_fns.h"
struct SvcObj;
#define NVIDEO_CONTEXTS 32
#define NVIDEO_CODECS   16
#define NVIDEO_POOLS    32
#define NVIDEO_MAPS     256
#define NVIDEO_IMAGES   8
#define NVIDEO_INPUTS   3
/* Flat service state. Codec packets and FFmpeg objects live only in HostNativeVideo, under its lock. */
typedef struct {
    u32 obj, encoder;
} NativeVideoContext;
typedef struct {
    u32 id, fence, available;
} NativeVideoImage;
typedef struct {
    u32 obj, encoder, tmem, event, width, height, capacity, started, syncpoint, next_fence, issued;
    u32 input_head, input_count, output_head, output_count, eos;
    u64 input_bytes, eos_timestamp;
    NativeVideoImage images[NVIDEO_IMAGES];
    u32 output[NVIDEO_IMAGES][16];
} NativeVideoCodec;
typedef struct {
    u32 first, count, memory, extent, pitch, chroma_offset, image_bytes, luma_bytes, chroma_bytes, retired;
    u64 cpu;
} NativeVideoPool;
typedef struct {
    u32 image, address_space, refs;
    u64 address, size;
} NativeVideoMap;
typedef struct {
    u32 decoder_ready, encoder_ready;
    NativeVideoContext contexts[NVIDEO_CONTEXTS];
    NativeVideoCodec codecs[NVIDEO_CODECS];
    NativeVideoPool pools[NVIDEO_POOLS];
    NativeVideoMap maps[NVIDEO_MAPS];
    u32 context_next, codec_next;
} HleNativeVideo;
typedef struct {
    struct AVCodecContext* decoder;
    struct AVFrame* frame;
    struct AVPacket* packets[NVIDEO_INPUTS];
} NativeVideoHostCodec;
typedef struct {
    PMutex lock;
    NativeVideoHostCodec codecs[NVIDEO_CODECS];
} HostNativeVideo;
struct SvcObj* native_video_context_new(int encoder);
int native_video_service(int encoder, int enable);
int native_video_requirements(const u32 preference[16], u32 count, u64* size, u64* alignment);
int native_video_pool_create(const u32 preference[16], u32 count, u32 memory, u32* image);
int native_video_pool_destroy(u32 image);
int native_video_query(u32 image, u64 descriptor[16]);
int native_video_query_gpu(u32 image, u64 descriptor[16]);
int native_video_map(u32 image, u32 address_space, u64* address, u64* last, u64 descriptor[16]);
int native_video_unmap(u32 image, u32 address_space, u64 address, u64 last);
/* Native video packets, reference pictures and live image pools are not part of save states. */
int native_video_busy(const HleNativeVideo* state);
void native_video_state_drop(void);
extern const StateFnTable g_native_video_state_fns;
#endif
