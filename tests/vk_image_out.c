/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Graphics storage images' write-backs (gpu/vk/image_out.c) against a stand-in GPU: a draw's writes change the image
   at once, a release records each image's copy into a ring and submits, and a submit is done when the test says so or
   something waits for it. A draw over an image whose write-back is recorded and not landed goes on from it, with no
   read of guest memory and no wait: the write-backs land in order, the later one over the bytes the earlier one left.
   A read of the bytes before then lands both in order; another surface's write-back over the same bytes, a GPU-side
   write since the copy or another size reads guest memory instead, as before */
#include "nx/gpu/gpu.h"
#include "nx/gpu/renderer.h"
#include "nx/gpu/vk/vk.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/log.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BASE 0x10000ull /* the images' guest bytes, at the same address on the GPU */
#define SIZE 64u

/* ---- guest memory and the GPU, as the queue sees them */
static struct {
    u8 guest[SIZE];
    Surf surf[2];
    u8 image[2][SIZE]; /* each surface's image on the GPU */
    u8 ring[1 << 16];
    u64 ring_at;
    u64 recording, done; /* the submit being recorded, the last one done */
    int waits, reads, syncs, dirty;
    u64 gen;
    char order[16]; /* each landing's surface, by its index as a letter */
    int landed;
    u8 last_original[SIZE]; /* the bytes from before the draws the last landing was given */
} g;
VkState g_vk;

static u8* image_of(const Surf* s) { return g.image[s - g.surf]; }
static void note_landing(const Surf* s, const u8* original) {
    if (g.landed < (int)sizeof g.order - 1) g.order[g.landed++] = (char)('A' + (s - g.surf));
    memcpy(g.last_original, original, SIZE);
}

u64 gmmu_run(u64 gva, u64 max, u64* cpu) {
    *cpu = gva;
    return max;
}
int gmem_read(u64 gva, void* dst, u64 size) {
    vk_gfx_image_mem_need(gva, size); /* the renderer's hook, before any read of guest memory */
    memcpy(dst, g.guest + (gva - BASE), size);
    g.reads++;
    return 1;
}
u64 gpu_mem_generation(void) { return g.gen; }
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz) {
    (void)a, (void)sz;
    return g.dirty && since < g.gen;
}
void vk_lock(void) {}
void vk_unlock(void) {}
VkCommandBuffer vk_cmd(void) { return VK_NULL_HANDLE; }
u8* vk_stage_back(u64 size, VkBuffer* buf, u64* off, VmaAllocation* mem) {
    if (g.ring_at + size > sizeof g.ring) return NULL;
    *buf = VK_NULL_HANDLE;
    *mem = VK_NULL_HANDLE;
    *off = g.ring_at;
    g.ring_at += size;
    return g.ring + *off;
}
u64 vk_surf_flush_size(const Surf* s) { return s->gva ? SIZE : 0; }
/* the copy, run in order with the draws: the image as it is now */
void vk_surf_flush_record(Surf* s, VkBuffer buf, u64 at) {
    (void)buf;
    memcpy(g.ring + at, image_of(s), SIZE);
}
u64 vk_submit_number(void) { return g.recording; }
u64 vk_last_submit(void) { return g.recording - 1; }
void vk_submit(VkSemaphore wait, VkPipelineStageFlags2 wait_stage, VkSemaphore signal) {
    (void)wait, (void)wait_stage, (void)signal;
    g.recording++;
}
void vk_wait_submit(u64 value) {
    if (value > g.done) {
        g.waits++;
        g.done = value;
    }
}
VkResult vmaInvalidateAllocation(VmaAllocator allocator, VmaAllocation allocation, VkDeviceSize offset,
                                 VkDeviceSize size) {
    (void)allocator, (void)allocation, (void)offset, (void)size;
    return VK_SUCCESS;
}
/* as surface.c's for a linear image of the guest's size: the bytes the image changed land, and keep gets the image */
void vk_surf_flush_land(Surf* s, const u8* p, const u8* original, u64 wgen, u8** keep, u64* keep_size) {
    if (!original) {
        fprintf(stderr, "a storage image landed without its bytes from before its draws\n");
        exit(2);
    }
    for (u32 i = 0; i < SIZE; i++)
        if (p[i] != original[i]) g.guest[i] = p[i];
    s->mem_wgen = wgen;
    note_landing(s, original);
    if (keep) {
        *keep = malloc(SIZE);
        if (!*keep) exit(2);
        memcpy(*keep, p, SIZE);
        *keep_size = SIZE;
    }
}
/* the copy recorded, submitted and waited for at once */
void vk_surf_flush(Surf* s, const u8* original) {
    const u8* p = image_of(s);
    for (u32 i = 0; i < SIZE; i++)
        if (p[i] != original[i]) g.guest[i] = p[i];
    s->mem_wgen = s->wgen;
    note_landing(s, original);
    g.syncs++;
}
void logf_(int level, const char* fmt, ...) {
    va_list ap;
    (void)level;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(2);
}

/* ---- the renderer's side, as vk/render.c's draw setup and commit do it */
static void draw(Surf* s, u32 size, u32 at, u8 value) {
    u8* original = NULL;
    u64 after = 0;
    if (!vk_gfx_image_unrecorded(s) && !(after = vk_gfx_image_after(s, size))) {
        original = calloc(1, SIZE); /* the stand-in GPU lands whole images, whatever the draw's size */
        if (!original) exit(2);
        gmem_read(s->gva, original, size);
    }
    image_of(s)[at] = value;
    s->wgen++;
    if (original || after) vk_gfx_image_record(s, original, size, after);
}
static void release(void) {
    vk_gfx_image_put_aside();
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
}
static void frame_done(void) {
    g.done = g.recording - 1;
    vk_gfx_image_frame_done(g.done);
}

static void start(void) {
    vk_gfx_image_land();
    memset(&g, 0, sizeof g);
    g.recording = 1;
    g.surf[0].gva = g.surf[1].gva = BASE;
}
static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

int main(void) {
    Surf* a = &g.surf[0];
    Surf* b = &g.surf[1];

    /* three draws a release apart: one read of guest memory, no wait, landed in order, each over the last's bytes */
    start();
    draw(a, SIZE, 1, 0x11);
    release();
    draw(a, SIZE, 2, 0x22);
    release();
    draw(a, SIZE, 3, 0x33);
    check(g.reads == 1 && g.waits == 0, "draws over a recorded write-back read guest memory once and wait for nothing");
    release();
    frame_done();
    check(!strcmp(g.order, "AAA"), "every write-back lands");
    check(g.guest[1] == 0x11 && g.guest[2] == 0x22 && g.guest[3] == 0x33, "with every draw's writes");
    check(g.last_original[1] == 0x11 && g.last_original[2] == 0x22 && !g.last_original[3],
          "the last over the bytes the one before it left");
    check(g.waits == 0 && g.syncs == 0 && !g_gfx_img_any, "and none is left");

    /* a read before the first lands: it waits for that copy, then the draw after it is copied at once */
    start();
    draw(a, SIZE, 1, 0x11);
    release();
    draw(a, SIZE, 2, 0x22);
    {
        u8 seen[SIZE];
        gmem_read(BASE, seen, SIZE);
        check(seen[1] == 0x11 && seen[2] == 0x22, "a read sees both draws' writes");
    }
    check(!strcmp(g.order, "AA") && g.waits == 1 && g.syncs == 1, "the recorded one landed first, with one wait");
    check(g.last_original[1] == 0x11 && !g.last_original[2], "the second over the first's bytes");

    /* a read once both copies are recorded: both land, in order, waiting for the second's submit */
    start();
    draw(a, SIZE, 1, 0x11);
    release();
    draw(a, SIZE, 2, 0x22);
    release();
    vk_gfx_image_mem_need(BASE, SIZE);
    check(!strcmp(g.order, "AA") && g.guest[1] == 0x11 && g.guest[2] == 0x22 && g.syncs == 0, "both land in order");
    check(!g_gfx_img_any, "and none is left");

    /* another surface's write-back over the same bytes: each draw reads guest memory, which lands the one before */
    start();
    draw(a, SIZE, 1, 0x11);
    release();
    draw(b, SIZE, 2, 0x22);
    release();
    draw(a, SIZE, 3, 0x33);
    check(g.reads == 3 && !strcmp(g.order, "AB") && g.waits == 2, "another surface's write-back isn't gone on from");

    /* a GPU-side write over the bytes since the copy: guest memory is read */
    start();
    draw(a, SIZE, 1, 0x11);
    release();
    g.gen++;
    g.dirty = 1;
    draw(a, SIZE, 2, 0x22);
    check(g.reads == 2 && !strcmp(g.order, "A") && g.waits == 1, "a GPU-side write since the copy");

    /* the draw writes fewer bytes of it: guest memory is read */
    start();
    draw(a, SIZE, 1, 0x11);
    release();
    draw(a, SIZE / 2, 2, 0x22);
    check(g.reads == 2 && !strcmp(g.order, "A") && g.waits == 1, "a draw of another size");

    /* a write-back recorded in the submit being recorded lands with the frame that submits it */
    start();
    draw(a, SIZE, 1, 0x11);
    release();
    draw(a, SIZE, 2, 0x22);
    frame_done();
    check(!strcmp(g.order, "A") && g.waits == 0, "the first lands with its frame");
    release();
    frame_done();
    check(!strcmp(g.order, "AA") && g.guest[2] == 0x22 && g.last_original[1] == 0x11, "the second with the next");

    vk_gfx_image_land();
    if (failures) return 1;
    printf("graphics storage image write-backs: in order, over the last one's bytes, with no wait\n");
    return 0;
}
