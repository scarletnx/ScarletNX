/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Ounce's native video service, its parts without state (src/nx/hle/services/native_video_core.c): the MIME
   types, the image layouts and requirements (991), the descriptors for the CPU (961) and the GPU (426, 428), the
   pools' image numbers, and a codec's state machine: the image queue and its fences (311, 314, 212), the output ring
   (213) and the input ring (109) */
#include "nx/hle/services/native_video.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* a preference as MakeDecodeImagePreference makes it: 0, the kind, the extent, 13 zeros */
static void preference(u32 pref[16], u32 kind, u32 w, u32 h) {
    memset(pref, 0, 16 * sizeof *pref);
    pref[1] = kind;
    pref[2] = (w - 1) | (h - 1) << 16;
}

static void check_mime(void) {
    assert(nvideo_mime((const u8*)"video/vp9", 10) == NVIDEO_MIME_VP9); /* with its NUL, as the SDK sends it */
    assert(nvideo_mime((const u8*)"video/vp9", 9) == NVIDEO_MIME_VP9);
    assert(nvideo_mime((const u8*)"video/avc", 10) == NVIDEO_MIME_H264);
    assert(nvideo_mime((const u8*)"video/x-vnd.on2.vp8", 20) == NVIDEO_MIME_VP8);
    assert(nvideo_mime((const u8*)"video/av1", 10) == NVIDEO_MIME_AV1);
    assert(nvideo_mime((const u8*)"video/vp90", 10) == NVIDEO_MIME_NONE); /* a longer name */
    assert(nvideo_mime((const u8*)"video/vp", 8) == NVIDEO_MIME_NONE);    /* a shorter one */
    assert(nvideo_mime((const u8*)"video/vp9\0x", 11) == NVIDEO_MIME_NONE); /* bytes after the NUL */
}

static void check_layout(void) {
    u32 pref[16];
    NativeVideoLayout l;
    u64 size = 0;
    /* NV12 640x360: rows of 768 bytes (whole 256s), 368 rows (whole 16s), the chroma's 184 after them */
    preference(pref, 1, 640, 360);
    assert(nvideo_layout(pref, 3, &l, &size) == 0);
    assert(l.kind == 1 && l.extent == (639u | 359u << 16));
    assert(l.pitch == 768 && l.rows == 368 && l.chroma_rows == 184 && l.chroma_offset == 768u * 368);
    assert(l.image_bytes == 0x68000); /* 768 * 552 = 0x67800, whole pages */
    assert(size == 3ull * 0x68000);
    /* kind 4 is NV12 too. 1 x 1 takes 16 rows of 256 bytes and 8 chroma rows: 6144 bytes, two pages */
    preference(pref, 4, 1, 1);
    assert(nvideo_layout(pref, 1, &l, &size) == 0);
    assert(l.pitch == 256 && l.rows == 16 && l.chroma_rows == 8 && l.image_bytes == 8192 && size == 8192);
    /* 2560x1440, an opening movie's */
    preference(pref, 1, 2560, 1440);
    assert(nvideo_layout(pref, 8, &l, &size) == 0);
    assert(l.pitch == 2560 && l.rows == 1440 && l.chroma_offset == 2560u * 1440);
    assert(l.image_bytes == 0x546000 && size == 8ull * 0x546000);
    /* four bytes a pixel: no chroma plane */
    preference(pref, 2, 100, 50);
    assert(nvideo_layout(pref, 2, &l, &size) == 0);
    assert(l.pitch == 512 && l.rows == 64 && l.chroma_rows == 0 && l.chroma_offset == 0);
    assert(l.image_bytes == 512u * 64 && size == 2ull * 512 * 64);
    /* the largest extent the words hold: past the 32 bits of the descriptors' offsets, refused */
    preference(pref, 3, 65536, 65536);
    assert(nvideo_layout(pref, 1, &l, &size) == NVIDEO_BAD_ARGUMENT);
    preference(pref, 1, 8192, 8192);
    assert(nvideo_layout(pref, 2, &l, &size) == 0 && l.image_bytes == 8192u * 12288 && size == 2ull * 8192 * 12288);
    /* refused: count 0 or past 256, an unknown kind, a reserved word set */
    preference(pref, 1, 64, 64);
    assert(nvideo_layout(pref, 0, &l, &size) == NVIDEO_BAD_ARGUMENT);
    assert(nvideo_layout(pref, 257, &l, &size) == NVIDEO_BAD_ARGUMENT);
    assert(nvideo_layout(pref, 256, &l, &size) == 0);
    pref[1] = 7;
    assert(nvideo_layout(pref, 1, &l, &size) == NVIDEO_BAD_ARGUMENT);
    pref[1] = 0;
    assert(nvideo_layout(pref, 1, &l, &size) == NVIDEO_BAD_ARGUMENT);
    for (u32 i = 0; i < 16; i++) {
        if (i == 1 || i == 2) continue;
        preference(pref, 1, 64, 64);
        pref[i] = 1;
        assert(nvideo_layout(pref, 1, &l, &size) == NVIDEO_BAD_ARGUMENT);
    }
}

static void check_descriptors(void) {
    u32 pref[16];
    NativeVideoPool p = {.first = 10, .count = 4, .memory = 3, .cpu = 0x80000000ull};
    u64 size, d[16];
    preference(pref, 1, 640, 360);
    assert(nvideo_layout(pref, 4, &p.layout, &size) == 0);
    /* 961, the third image: NV12 (0xa) with its extent, luma at its offset in the memory with its plane's size and
       pitch, then the chroma */
    for (u32 i = 0; i < 16; i++) d[i] = ~0ull;
    nvideo_cpu_descriptor(&p, 12, d);
    assert(d[0] == (0xa | (u64)(639u | 359u << 16) << 32));
    assert(d[1] == (2ull * 0x68000 | (u64)768 * 368 << 32) && d[2] == 768);
    assert(d[3] == ((2ull * 0x68000 + 768 * 368) | (u64)768 * 184 << 32) && d[4] == 768);
    for (u32 i = 5; i < 16; i++) assert(!d[i]);
    /* the first image starts the memory */
    nvideo_cpu_descriptor(&p, 10, d);
    assert((u32)d[1] == 0 && (u32)d[3] == 768u * 368);
    /* four bytes a pixel: 0x9 for kinds 2 and 5, 0x8 for 3 and 6, one plane */
    preference(pref, 3, 100, 50);
    assert(nvideo_layout(pref, 4, &p.layout, &size) == 0);
    nvideo_cpu_descriptor(&p, 11, d);
    assert((d[0] & 0xff) == 0x8 && d[1] == (512ull * 64 | (u64)512 * 64 << 32) && d[2] == 512 && !d[3] && !d[4]);
    p.layout.kind = 5;
    nvideo_cpu_descriptor(&p, 11, d);
    assert((d[0] & 0xff) == 0x9);
    /* 428: the image's offset and size in the memory in d[5], the checked bytes of d[0] and d[4] */
    preference(pref, 1, 640, 360);
    assert(nvideo_layout(pref, 4, &p.layout, &size) == 0);
    nvideo_gpu_descriptor(&p, 13, d);
    assert((d[0] & 0xff) == 1 && (d[4] & 0xff) == 1 && d[1] == (639u | 359u << 16));
    assert(d[5] == ((u64)0x68000 << 32 | 3ull * 0x68000));
}

static void check_pools(void) {
    NativeVideoPool pools[4] = {{.first = 0, .count = 8}, /* free: never found */
                                {.first = 5, .count = 3},
                                {.first = 20, .count = 1}};
    assert(!nvideo_pool_find(pools, 4, 0));
    assert(!nvideo_pool_find(pools, 4, 4));
    assert(nvideo_pool_find(pools, 4, 5) == &pools[1] && nvideo_pool_find(pools, 4, 7) == &pools[1]);
    assert(!nvideo_pool_find(pools, 4, 8));
    assert(nvideo_pool_find(pools, 4, 20) == &pools[2] && !nvideo_pool_find(pools, 4, 21));
    assert(!nvideo_pool_find(pools, 1, 6)); /* only the first n pools */
}

static void check_codec_images(void) {
    NativeVideoCodec c = {0};
    u32 props[NVIDEO_PROPS] = {0}, f[NVIDEO_IMAGES + 1];
    /* the syncpoint stands at 0xfffffffe: the fences wrap past 0 */
    c.issued = c.next_fence = 0xfffffffeu;
    assert(nvideo_image_register(&c, 0, &f[0]) == NVIDEO_BAD_ARGUMENT);
    assert(nvideo_image_register(&c, 7, &f[0]) == 0 && f[0] == 0xffffffffu);
    assert(nvideo_image_register(&c, 7, &f[1]) == NVIDEO_BAD_STATE); /* the same image twice */
    assert(nvideo_image_register(&c, 8, &f[1]) == 0 && f[1] == 0);
    assert(nvideo_image_register(&c, 9, &f[2]) == 0 && f[2] == 1);
    /* the decoder writes the earliest fence first, across the wrap */
    NativeVideoImage* next = nvideo_image_next(&c);
    assert(next && next->id == 7);
    props[0] = 7;
    assert(nvideo_image_fill(&c, next, props) == 1 && c.issued == 0xffffffffu);
    assert(c.output_count == 1 && !next->queued);
    next = nvideo_image_next(&c);
    assert(next && next->id == 8);
    /* image 8 unregistered while queued: its fence is never reached, and 9's fill makes both increments */
    assert(nvideo_image_unregister(&c, 8) == 0);
    assert(nvideo_image_unregister(&c, 8) == NVIDEO_NOT_FOUND);
    next = nvideo_image_next(&c);
    assert(next && next->id == 9);
    props[0] = 9;
    assert(nvideo_image_fill(&c, next, props) == 2 && c.issued == 1);
    assert(!nvideo_image_next(&c)); /* none queued */
    /* the outputs in the order filled */
    assert(nvideo_output_pop(&c, props) == 0 && props[0] == 7);
    assert(nvideo_output_pop(&c, props) == 0 && props[0] == 9);
    assert(nvideo_output_pop(&c, props) == NVIDEO_OUTPUT_EMPTY);
    /* 314: image 7 back to the decoder after the game's fence, with a new fence */
    assert(nvideo_image_release(&c, 7, 12, 345, &f[3]) == 0 && f[3] == 2);
    next = nvideo_image_next(&c);
    assert(next && next->id == 7 && next->wait_id == 12 && next->wait_value == 345);
    /* released again while queued: the fence it has */
    assert(nvideo_image_release(&c, 7, 13, 999, &f[4]) == 0 && f[4] == 2 && next->wait_id == 12);
    assert(nvideo_image_release(&c, 8, ~0u, 0, &f[4]) == NVIDEO_NOT_FOUND);
    assert(nvideo_image_release(&c, 0, ~0u, 0, &f[4]) == NVIDEO_NOT_FOUND);
    props[0] = 7;
    assert(nvideo_image_fill(&c, next, props) == 1 && next->wait_id == ~0u);
    assert(nvideo_output_pop(&c, props) == 0 && props[0] == 7);
    /* the slots: NVIDEO_IMAGES at once, the next refused */
    NativeVideoCodec full = {0};
    for (u32 i = 0; i < NVIDEO_IMAGES; i++) assert(nvideo_image_register(&full, 100 + i, &f[i]) == 0);
    assert(nvideo_image_register(&full, 200, &f[0]) == NVIDEO_NO_ROOM);
    assert(nvideo_image_unregister(&full, 105) == 0);
    assert(nvideo_image_register(&full, 200, &f[0]) == 0 && f[0] == NVIDEO_IMAGES + 1);
    /* a full output ring: the decoder writes no image until the client takes an output */
    for (u32 i = 0; i < NVIDEO_OUTPUTS; i++) {
        next = nvideo_image_next(&full);
        assert(next);
        props[0] = next->id;
        nvideo_image_fill(&full, next, props);
    }
    assert(full.output_count == NVIDEO_OUTPUTS);
    assert(nvideo_image_release(&full, 100, ~0u, 0, &f[0]) == 0);
    assert(!nvideo_image_next(&full));
    assert(nvideo_output_pop(&full, props) == 0 && props[0] == 100);
    next = nvideo_image_next(&full);
    assert(next && next->id == 100);
}

static void check_inputs(void) {
    NativeVideoCodec c = {0};
    for (u32 i = 0; i < NVIDEO_INPUTS; i++) assert(nvideo_input_push(&c, 100 + i, 1000 * i, 0) == (int)i);
    assert(nvideo_input_push(&c, 1, 0, 1) == -1);
    nvideo_input_pop(&c);
    nvideo_input_pop(&c);
    assert(c.input_count == NVIDEO_INPUTS - 2 && c.inputs[c.input_head].size == 102);
    /* the ring wraps: the new packets take the slots the first two left */
    assert(nvideo_input_push(&c, 7, 5, 0) == 0 && nvideo_input_push(&c, 0, 6, 1) == 1);
    assert(nvideo_input_push(&c, 1, 0, 0) == -1);
    for (u32 i = 2; i < NVIDEO_INPUTS; i++) nvideo_input_pop(&c);
    assert(c.input_head == 0 && c.inputs[0].size == 7 && c.inputs[1].eos == 1 && c.inputs[1].timestamp == 6);
    nvideo_input_pop(&c);
    nvideo_input_pop(&c);
    assert(!c.input_count);
    nvideo_input_pop(&c); /* empty: nothing */
    assert(!c.input_count && c.input_head == 2);
}

static void check_props(void) {
    u32 props[NVIDEO_PROPS];
    for (u32 i = 0; i < NVIDEO_PROPS; i++) props[i] = ~0u;
    /* the timestamp in microseconds goes out in nanoseconds, low word first */
    nvideo_props(props, 42, 640, 360, 638, 358, 5000000123ull, 0);
    assert(props[0] == 42 && props[1] == 640 && props[2] == 360 && props[3] == 638 && props[4] == 358);
    assert(!props[5] && ((u64)props[7] << 32 | props[6]) == 5000000123000ull && !props[8]);
    for (u32 i = 9; i < NVIDEO_PROPS; i++) assert(!props[i]);
    nvideo_props(props, 1, 2, 3, 2, 3, 0, 1);
    assert(props[8] == 1 && !props[6] && !props[7]);
    nvideo_props(props, 1, 2, 3, 2, 3, 0, 5); /* any flag: 1 */
    assert(props[8] == 1);
}

int main(void) {
    check_mime();
    check_layout();
    check_descriptors();
    check_pools();
    check_codec_images();
    check_inputs();
    check_props();
    puts("native_video: ok");
    return 0;
}
