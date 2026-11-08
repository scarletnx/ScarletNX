/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native video service's parts that hold no state (native_video.h): the images' layouts and descriptors, a
   codec's image queue and fences, and its input and output rings. The command numbers and the descriptors' fields
   follow the game's SDK (nn::codec's decoders, nn::gs::sdk::ReadUserImageNv12 and nn::gpu::sdk::MapNativeImage),
   with closed PR #1009 as a reference. The image layout itself is the
   service's choice: the SDK sizes a pool with 991 and reads an image only through the descriptors */
#include "nx/hle/services/native_video.h"
#include "common/utilities/bits.h"
#include <string.h>

u32 nvideo_mime(const u8* p, u64 size) {
    static const struct {
        const char* name;
        u32 mime;
    } names[] = {{"video/vp9", NVIDEO_MIME_VP9}, {"video/x-vnd.on2.vp8", NVIDEO_MIME_VP8},
                 {"video/vp8", NVIDEO_MIME_VP8}, {"video/avc", NVIDEO_MIME_H264}, {"video/av1", NVIDEO_MIME_AV1}};
    for (u32 i = 0; i < sizeof names / sizeof *names; i++) {
        size_t n = strlen(names[i].name);
        if ((size == n || (size == n + 1 && !p[n])) && !memcmp(p, names[i].name, n)) return names[i].mime;
    }
    return NVIDEO_MIME_NONE;
}

int nvideo_layout(const u32 pref[16], u32 count, NativeVideoLayout* l, u64* size) {
    R_UNLESS(!pref[0] && count && count <= 256, NVIDEO_BAD_ARGUMENT);
    for (u32 i = 3; i < 16; i++) R_UNLESS(!pref[i], NVIDEO_BAD_ARGUMENT);
    u32 kind = pref[1], w = (pref[2] & 0xffff) + 1, h = (pref[2] >> 16) + 1;
    int nv12 = nvideo_kind_nv12(kind);
    R_UNLESS(nv12 || kind == 2 || kind == 3 || kind == 5 || kind == 6, NVIDEO_BAD_ARGUMENT);
    NativeVideoLayout r = {.kind = kind, .extent = pref[2]};
    r.pitch = (u32)snx_align_up((u64)w * (nv12 ? 1 : 4), 256);
    r.rows = (u32)snx_align_up(h, 16);
    r.chroma_rows = nv12 ? r.rows / 2 : 0;
    u64 bytes = snx_align_up((u64)r.pitch * (r.rows + r.chroma_rows), 4096);
    R_UNLESS(bytes <= 0xffffffffull, NVIDEO_BAD_ARGUMENT); /* the descriptors hold an image's offsets in 32 bits */
    r.chroma_offset = nv12 ? r.pitch * r.rows : 0;
    r.image_bytes = (u32)bytes;
    *l = r;
    *size = (u64)r.image_bytes * count;
    R_SUCCEED();
}

void nvideo_cpu_descriptor(const NativeVideoPool* p, u32 image, u64 d[16]) {
    const NativeVideoLayout* l = &p->layout;
    u64 at = (u64)(image - p->first) * l->image_bytes;
    memset(d, 0, 16 * sizeof *d);
    if (nvideo_kind_nv12(l->kind)) {
        d[0] = 0xa | (u64)l->extent << 32;
        d[1] = at | (u64)l->pitch * l->rows << 32;
        d[2] = l->pitch;
        d[3] = (at + l->chroma_offset) | (u64)l->pitch * l->chroma_rows << 32;
        d[4] = l->pitch;
    } else {
        d[0] = (l->kind == 2 || l->kind == 5 ? 0x9 : 0x8) | (u64)l->extent << 32;
        d[1] = at | (u64)l->pitch * l->rows << 32;
        d[2] = l->pitch;
    }
}
void nvideo_gpu_descriptor(const NativeVideoPool* p, u32 image, u64 d[16]) {
    const NativeVideoLayout* l = &p->layout;
    memset(d, 0, 16 * sizeof *d);
    d[0] = 1; /* MapNativeImage checks the low byte of d[0] and of d[4] is 1, and that d[5] has a size */
    d[1] = l->extent;
    d[4] = 0x101000001ull;
    d[5] = (u64)l->image_bytes << 32 | (u64)(image - p->first) * l->image_bytes;
}
NativeVideoPool* nvideo_pool_find(NativeVideoPool* pools, u32 n, u32 image) {
    for (u32 i = 0; i < n; i++)
        if (pools[i].first && image >= pools[i].first && image - pools[i].first < pools[i].count) return &pools[i];
    return NULL;
}

static NativeVideoImage* image_of(NativeVideoCodec* c, u32 id) {
    for (u32 i = 0; i < NVIDEO_IMAGES; i++)
        if (c->images[i].id == id) return &c->images[i];
    return NULL;
}
int nvideo_image_register(NativeVideoCodec* c, u32 id, u32* fence) {
    R_UNLESS(id, NVIDEO_BAD_ARGUMENT);
    R_UNLESS(!image_of(c, id), NVIDEO_BAD_STATE);
    NativeVideoImage* slot = image_of(c, 0);
    R_UNLESS(slot, NVIDEO_NO_ROOM);
    *slot = (NativeVideoImage){.id = id, .fence = ++c->next_fence, .queued = 1, .wait_id = ~0u};
    *fence = slot->fence;
    R_SUCCEED();
}
int nvideo_image_release(NativeVideoCodec* c, u32 id, u32 wait_id, u32 wait_value, u32* fence) {
    NativeVideoImage* image = id ? image_of(c, id) : NULL;
    R_UNLESS(image, NVIDEO_NOT_FOUND);
    /* the SDK releases an image it holds, once: one still queued keeps its fence */
    if (!image->queued) {
        image->queued = 1;
        image->fence = ++c->next_fence;
        image->wait_id = wait_id;
        image->wait_value = wait_value;
    }
    *fence = image->fence;
    R_SUCCEED();
}
int nvideo_image_unregister(NativeVideoCodec* c, u32 id) {
    NativeVideoImage* image = id ? image_of(c, id) : NULL;
    R_UNLESS(image, NVIDEO_NOT_FOUND);
    *image = (NativeVideoImage){0};
    R_SUCCEED();
}
NativeVideoImage* nvideo_image_next(NativeVideoCodec* c) {
    NativeVideoImage* next = NULL;
    if (c->output_count == NVIDEO_OUTPUTS) return NULL;
    for (u32 i = 0; i < NVIDEO_IMAGES; i++) {
        NativeVideoImage* q = &c->images[i];
        if (q->id && q->queued && (!next || q->fence - c->issued < next->fence - c->issued)) next = q;
    }
    return next;
}
u32 nvideo_image_fill(NativeVideoCodec* c, NativeVideoImage* image, const u32 props[NVIDEO_PROPS]) {
    u32 inc = image->fence - c->issued;
    c->issued = image->fence;
    image->queued = 0;
    image->wait_id = ~0u;
    memcpy(c->outputs[(c->output_head + c->output_count++) % NVIDEO_OUTPUTS], props, NVIDEO_PROPS * sizeof *props);
    return inc;
}
int nvideo_output_pop(NativeVideoCodec* c, u32 props[NVIDEO_PROPS]) {
    R_UNLESS(c->output_count, NVIDEO_OUTPUT_EMPTY);
    memcpy(props, c->outputs[c->output_head], NVIDEO_PROPS * sizeof *props);
    c->output_head = (c->output_head + 1) % NVIDEO_OUTPUTS;
    c->output_count--;
    R_SUCCEED();
}
int nvideo_input_push(NativeVideoCodec* c, u32 size, u64 timestamp, u32 eos) {
    if (c->input_count == NVIDEO_INPUTS) return -1;
    u32 slot = (c->input_head + c->input_count++) % NVIDEO_INPUTS;
    c->inputs[slot] = (NativeVideoInput){timestamp, size, eos};
    return (int)slot;
}
void nvideo_input_pop(NativeVideoCodec* c) {
    if (!c->input_count) return;
    c->inputs[c->input_head] = (NativeVideoInput){0};
    c->input_head = (c->input_head + 1) % NVIDEO_INPUTS;
    c->input_count--;
}
void nvideo_props(u32 props[NVIDEO_PROPS], u32 image, u32 width, u32 height, u32 pic_w, u32 pic_h, u64 timestamp_us,
                  int eos) {
    u64 ns = timestamp_us * 1000;
    memset(props, 0, NVIDEO_PROPS * sizeof *props);
    props[0] = image;
    props[1] = width;
    props[2] = height;
    props[3] = pic_w;
    props[4] = pic_h;
    props[5] = 0; /* the colour space: BT.601 */
    props[6] = (u32)ns;
    props[7] = (u32)(ns >> 32);
    props[8] = eos != 0;
}
