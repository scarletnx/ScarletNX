/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Ounce's native video service, which the graphics supervisor (gsv.c) serves: the decoder and encoder contexts
   (901 to 906, 1001 to 1003), their codec sessions, and the video image pools the decoders write into (911, 912,
   961, 991, and 426 to 428 for the GPU). native_video.c is the service and FFmpeg, native_video_core.c the layouts,
   the descriptors and the codec's image queue, which hold no state of their own (tests/native_video.c) */
#ifndef SNX_NATIVE_VIDEO_H
#define SNX_NATIVE_VIDEO_H
#include "nx/runtime/state_fns.h"
#include "common/cpu/aarch64/cpu.h"
#include "nx/hle/result.h"
#include "nx/runtime/platform/plat.h" /* PMutex */

#define NVIDEO_POOLS    32
#define NVIDEO_MAPS     64
#define NVIDEO_CONTEXTS 8
#define NVIDEO_CODECS   8
/* the images a codec holds: the SDK's decoder has 8 slots, and its client queue aborts past 8 */
#define NVIDEO_IMAGES  16
#define NVIDEO_INPUTS  16 /* the packets queued before the decoder takes them */
#define NVIDEO_OUTPUTS NVIDEO_IMAGES
#define NVIDEO_PROPS   16 /* the words of an output's properties (213) */

/* the results the SDK's codec library knows (CODEC, result_codes.inc): Submit's queue full, which the movie library
   retries, and no output yet, which its own Acquire also answers */
#define NVIDEO_INPUT_FULL   SNX_RC_CODEC_INPUT_FULL
#define NVIDEO_OUTPUT_EMPTY SNX_RC_CODEC_OUTPUT_EMPTY
#define NVIDEO_BAD_ARGUMENT SNX_RC_CODEC_BAD_ARGUMENT
#define NVIDEO_BAD_STATE    SNX_RC_CODEC_BAD_STATE
#define NVIDEO_NOT_FOUND    SNX_RC_CODEC_NOT_FOUND
#define NVIDEO_NO_ROOM      SNX_RC_CODEC_NO_ROOM

/* the SDK's MakeDecodeImagePreference kinds: 1 and 4 NV12 (8 bit), which the decoder writes, and 2, 3, 5 and 6 four
   bytes a pixel */
static inline int nvideo_kind_nv12(u32 kind) { return kind == 1 || kind == 4; }

/* one image of a pool, as the preference describes it: rows pitch bytes apart, the chroma plane (NV12) from
   chroma_offset, image_bytes from one image to the next (whole pages) */
typedef struct {
    u32 kind, extent; /* extent: (width - 1) | (height - 1) << 16 */
    u32 pitch, rows, chroma_offset, chroma_rows, image_bytes;
} NativeVideoLayout;
/* a pool: count images, numbered first on (gsv's native image numbers), in the native memory at cpu. first 0: free */
typedef struct {
    u32 first, count, memory;
    u64 cpu;
    NativeVideoLayout layout;
} NativeVideoPool;
/* an image mapped for the GPU (426) in the supervisor's partition, refs times. image 0: free */
typedef struct {
    u32 image, address_space, refs;
    u64 address, size;
} NativeVideoMap;
/* a codec's image: fence is the value of the codec's syncpoint that tells it holds a picture, and queued that the
   decoder may write it. wait_id and wait_value: the fence the game gave at its release, ~0 for none */
typedef struct {
    u32 id, fence, queued, wait_id, wait_value;
} NativeVideoImage;
enum { NVIDEO_NEW, NVIDEO_INITIALIZED, NVIDEO_CONFIGURED };
/* an input packet's bookkeeping. The bytes are FFmpeg's packet in g_host */
typedef struct {
    u64 timestamp; /* microseconds, as Submit sends it */
    u32 size, eos;
} NativeVideoInput;
typedef struct {
    u32 obj;
    u32 encoder, state, started;
    /* eos: 0 none, 1 the decoder drains to the stream's end, 2 the end reached and its output due */
    u32 eos;
    u64 eos_timestamp;
    u32 codec, input_format, output_format, mime; /* mime: NVIDEO_MIME_* */
    u32 tmem;                                      /* the work memory's kernel object id, 0 for a process's */
    u64 work_size;
    u32 width, height, frame_rate, capacity;
    u32 event; /* the submission event's kernel object id */
    u32 syncpoint, issued, next_fence;
    u32 input_head, input_count;
    NativeVideoInput inputs[NVIDEO_INPUTS];
    NativeVideoImage images[NVIDEO_IMAGES];
    u32 output_head, output_count;
    u32 outputs[NVIDEO_OUTPUTS][NVIDEO_PROPS];
    u32 frames; /* pictures written, for the log */
} NativeVideoCodec;
typedef struct {
    u32 obj;
    u32 encoder;
} NativeVideoContext;
/* ---- HLE state in g_hle (hle.h) */
typedef struct {
    u32 decoder_ready, encoder_ready;
    NativeVideoPool pools[NVIDEO_POOLS];
    NativeVideoMap maps[NVIDEO_MAPS];
    NativeVideoContext contexts[NVIDEO_CONTEXTS];
    NativeVideoCodec codecs[NVIDEO_CODECS];
    u32 context_next, codec_next;
    u64 va_next; /* the next GPU address 426 hands out, in the supervisor's partition */
} HleNativeVideo;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
/* a codec's decoder (native_video_decoder.c): how it decodes, and for AV1 with Vulkan Video the packets it took since
   its last picture, which the CPU's decoder starts from when the Vulkan one refuses the stream */
enum { NVIDEO_DECODER_NONE, NVIDEO_DECODER_CPU, NVIDEO_DECODER_VULKAN, NVIDEO_DECODER_BLACK };
#define NVIDEO_REPLAY NVIDEO_INPUTS
typedef struct {
    struct AVCodecContext* ctx; /* FFmpeg's decoder: NULL for the black stand-in */
    u32 way;                    /* NVIDEO_DECODER_* */
    u32 index;                  /* the codec's, for the log */
    u32 width, height;          /* the configured size, the stand-in's pictures' */
    u32 pictures;               /* out since it was opened or flushed */
    struct AVPacket* replay[NVIDEO_REPLAY];
    u32 nreplay, replayed; /* the packets kept, and those of them the decoder that took over has taken */
    /* the stand-in: its picture, the timestamp of the input one is due for, and the end of the stream sent */
    struct AVFrame* black;
    i64 black_pts;
    u32 black_due, black_end;
} NativeVideoDecoder;
/* the host's side, which a save state never holds: the lock over every command, and each codec's FFmpeg objects (the
   same index as g_hle's codecs) */
typedef struct {
    PMutex lock;
    struct {
        NativeVideoDecoder decoder;
        struct AVFrame* frame; /* a picture received and not yet written: frame->buf[0] set */
        struct AVPacket* packets[NVIDEO_INPUTS];
        u32 black_images[NVIDEO_IMAGES], nblack; /* the images the stand-in painted black already, which stay so */
    } codecs[NVIDEO_CODECS];
} HostNativeVideo;
extern const StateFnTable g_native_video_state_fns; /* the functions a save state may hold of native_video.c */

/* ---- native_video_core.c */
enum { NVIDEO_MIME_NONE, NVIDEO_MIME_VP9, NVIDEO_MIME_VP8, NVIDEO_MIME_H264, NVIDEO_MIME_AV1 };
/* the codec a MIME type names (the SDK's holders send "video/vp9" with its NUL), NVIDEO_MIME_NONE for others */
u32 nvideo_mime(const u8* p, u64 size);
/* the layout of the images a preference (16 words: 0, the kind, the extent, 13 zeros) describes, and the bytes count
   of them take: 0, or NVIDEO_BAD_ARGUMENT */
int nvideo_layout(const u32 pref[16], u32 count, NativeVideoLayout* l, u64* size);
/* image's descriptor for the CPU (961, which nn::gs::sdk::ReadUserImageNv12 reads: format byte 0xa, the luma at
   d[1]'s low word with pitch d[2], the chroma at d[3]'s low word with pitch d[4], offsets in the pool's memory) */
void nvideo_cpu_descriptor(const NativeVideoPool* p, u32 image, u64 d[16]);
/* image's descriptor for the GPU (426 and 428): its offset in the memory in d[5]'s low word, its size in the high */
void nvideo_gpu_descriptor(const NativeVideoPool* p, u32 image, u64 d[16]);
/* the pool image belongs to, NULL for none */
NativeVideoPool* nvideo_pool_find(NativeVideoPool* pools, u32 n, u32 image);
/* RegisterImage and ReleaseImage: the image queued for the decoder with the next fence, *fence its value */
int nvideo_image_register(NativeVideoCodec* c, u32 id, u32* fence);
int nvideo_image_release(NativeVideoCodec* c, u32 id, u32 wait_id, u32 wait_value, u32* fence);
int nvideo_image_unregister(NativeVideoCodec* c, u32 id);
/* the image the decoder writes next: the queued one with the earliest fence, as the client acquires them. NULL when
   none is queued */
NativeVideoImage* nvideo_image_next(NativeVideoCodec* c);
/* image filled (props its output's properties, props[0] its id): the output queued, the image no longer, and the
   increments the syncpoint makes to reach its fence */
u32 nvideo_image_fill(NativeVideoCodec* c, NativeVideoImage* image, const u32 props[NVIDEO_PROPS]);
/* the oldest output's properties: 0, or NVIDEO_OUTPUT_EMPTY */
int nvideo_output_pop(NativeVideoCodec* c, u32 props[NVIDEO_PROPS]);
/* the input ring: push gives the slot of a new packet, or -1 when the ring is full */
int nvideo_input_push(NativeVideoCodec* c, u32 size, u64 timestamp, u32 eos);
void nvideo_input_pop(NativeVideoCodec* c);
/* an output's properties: the image, the configured size, the picture's size, the colour space, the timestamp in
   nanoseconds and the end of the stream */
void nvideo_props(u32 props[NVIDEO_PROPS], u32 image, u32 width, u32 height, u32 pic_w, u32 pic_h, u64 timestamp_us,
                  int eos);
/* ---- native_video_decoder.c */
/* the decoder for a MIME type's codec (NVIDEO_MIME_*) at the configured size, d written whole: 1, or 0 when FFmpeg's
   couldn't be opened. config: H.264's stream configuration (NULL for none). hw_device: for AV1, the renderer's device
   for Vulkan Video (Renderer.nvdec_hw_device, taken over), or NULL with no_hw saying why there is none, for the log */
int nvideo_decoder_open(NativeVideoDecoder* d, u32 index, u32 mime, u32 width, u32 height, const u8* config,
                        u64 config_size, void* hw_device, const char* no_hw);
void nvideo_decoder_close(NativeVideoDecoder* d);
/* avcodec_send_packet (NULL: the end of the stream), avcodec_receive_frame and avcodec_flush_buffers on the decoder in
   use. An AV1 stream the Vulkan decoder refuses goes on with the CPU's inside them, from the packets kept */
int nvideo_decoder_send(NativeVideoDecoder* d, const struct AVPacket* pkt);
int nvideo_decoder_receive(NativeVideoDecoder* d, struct AVFrame* frame);
void nvideo_decoder_flush(NativeVideoDecoder* d);
const char* nvideo_decoder_name(const NativeVideoDecoder* d); /* for the log: FFmpeg's name, or the way */
/* ---- native_video.c, for gsv.c's commands */
int native_video_service(int encoder, int enable);                            /* 901, 903, 1001, 1003 */
int native_video_open(int encoder, u32* session);                             /* 902, 905, 1002 */
int native_video_requirements(const u32 pref[16], u32 count, u64* size, u64* alignment); /* 991 */
int native_video_pool_create(const u32 pref[16], u32 count, u32 memory, u32* image);    /* 911 */
int native_video_pool_destroy(u32 image);                                              /* 912 */
/* an image of a pool, as nvnNativeImageInitialize makes an NVN memory pool of it: its bytes in guest memory, and the
   row pitch it is written with when it is four bytes a pixel (0 for NV12). 1, or 0 when no pool has the image */
int native_video_image_memory(u32 image, u64* cpu, u64* size, u32* linear_pitch);
int native_video_query(u32 image, u64 d[16]);                                          /* 961 */
int native_video_gpu_query(u32 image, u64 d[16]);                                      /* 428 */
int native_video_gpu_map(u32 image, u32 address_space, u64* first, u64* last, u64 d[16]); /* 426 */
int native_video_gpu_unmap(u32 image, u32 address_space, u64 first, u64 last);           /* 427 */
/* FFmpeg's picture f (4:2:0 planes of 8 bits, or a GPU image Vulkan Video decoded into) written into image id of pool
   p, as a codec writes its output: 1 when written. The codecs', and tests/vulkan_video_av1.inc's */
int native_video_image_write(const NativeVideoPool* p, u32 id, const struct AVFrame* f);
/* a codec is open: a save state can't hold FFmpeg's pictures */
int native_video_busy(void);

/* ---- gsv.c's, for native_video.c */
/* count native image numbers in a row, the first returned: 0 when none are left */
u32 gsv_native_image_reserve(u32 count);
/* the guest CPU memory a native memory is bound to (114 or 423): 1, or 0 when it is bound to none */
int gsv_native_memory_cpu(u32 memory, u64* cpu, u64* size);
#endif
