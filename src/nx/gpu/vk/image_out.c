/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/renderer.h"
#include "nx/gpu/vk/vk.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/tracy.h"

/* Fragment programs' storage images, written back to guest memory: each with its bytes from before the draw (original,
   for the merge with the CPU's writes since) and the guest memory its bytes are in. A release puts their write-back
   aside: their copy is recorded into the frame's host-readable ring (sub, the submit it is in, back, wgen the write
   generation it copies), and they land once that submit is done (vk_gfx_image_frame_done), as storage buffers' results
   do. Before, each release copied and waited for the GPU: half of one game's frame time in vk_finish. Whatever needs
   their memory sooner lands them at once, waiting for that submit when its copy is recorded */
typedef struct {
    u64 cpu, n;
} MemRun;
typedef struct {
    Surf* surf;
    u8* original;
    u32 size;
    MemRun* runs; /* the guest memory of its bytes, by address, apart: the write-back changes those bytes only */
    size_t nruns;
    u64 sub, back_off, back_size, wgen;
    u8* back;
    VmaAllocation back_mem;
} GfxImageOut;
static struct {
    GfxImageOut* out;
    size_t count, cap;
} g_image_out;
static _Thread_local int g_img_landing;
_Atomic int g_gfx_img_any;

static int mem_run_cmp(const void* a, const void* b) {
    u64 x = ((const MemRun*)a)->cpu, y = ((const MemRun*)b)->cpu;
    return (x > y) - (x < y);
}
/* surf has a write-back waiting whose copy isn't recorded yet: a later draw's writes are in it */
int vk_gfx_image_unrecorded(const Surf* surf) {
    for (size_t i = 0; i < g_image_out.count; i++)
        if (g_image_out.out[i].surf == surf && !g_image_out.out[i].sub) return 1;
    return 0;
}
void vk_gfx_image_record(Surf* surf, u8* original, u32 size) {
    if (vk_gfx_image_unrecorded(surf)) {
        free(original);
        return;
    }
    if (g_image_out.count == g_image_out.cap) {
        size_t cap = g_image_out.cap ? g_image_out.cap * 2 : 8;
        GfxImageOut* next = realloc(g_image_out.out, cap * sizeof *next);
        if (!next) fatal("Out of memory recording graphics storage image");
        g_image_out.out = next;
        g_image_out.cap = cap;
    }
    /* at most a run a page */
    size_t max_runs = (size_t)(((surf->gva & GMMU_PAGE_MASK) + (u64)size + GMMU_PAGE_MASK) >> GMMU_PAGE_BITS);
    MemRun* runs = malloc((max_runs ? max_runs : 1) * sizeof *runs);
    if (!runs) fatal("Out of memory recording graphics storage image memory");
    size_t nruns = 0;
    for (u64 off = 0; off < size;) {
        u64 cpu = 0, n = gmmu_run(surf->gva + off, size - off, &cpu);
        if (!n) { /* an unmapped page: the write-back writes nothing there */
            u64 left = GMMU_PAGE_SIZE - ((surf->gva + off) & GMMU_PAGE_MASK);
            off += left < size - off ? left : size - off;
            continue;
        }
        runs[nruns++] = (MemRun){cpu, n};
        off += n;
    }
    qsort(runs, nruns, sizeof *runs, mem_run_cmp);
    size_t kept = 0; /* runs that meet or touch made one, so their ends rise with their starts (image_meets) */
    for (size_t i = 0; i < nruns; i++)
        if (kept && runs[kept - 1].cpu + runs[kept - 1].n >= runs[i].cpu) {
            u64 end = runs[i].cpu + runs[i].n;
            if (end > runs[kept - 1].cpu + runs[kept - 1].n) runs[kept - 1].n = end - runs[kept - 1].cpu;
        } else
            runs[kept++] = runs[i];
    nruns = kept;
    g_image_out.out[g_image_out.count++] =
        (GfxImageOut){.surf = surf, .original = original, .size = size, .runs = runs, .nruns = nruns};
    g_gfx_img_any = 1;
}
/* [cpu, cpu + n) meets the image's bytes: the first run ending past cpu starts before cpu + n */
static int image_meets(const GfxImageOut* out, u64 cpu, u64 n) {
    size_t lo = 0, hi = out->nruns;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (out->runs[mid].cpu + out->runs[mid].n <= cpu)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < out->nruns && out->runs[lo].cpu < cpu + n;
}
/* [gva, gva + size) meets the image's bytes, through its own address or another mapping of the same memory. By the
   byte, not the page: NVN packs textures closer than a page, and one game's menu hashed a 480x270 texture ending in
   the page its twin storage image starts in, every frame, landing the image with a GPU wait each time (a fifth of the
   run in vk_finish). The write-back changes the image's bytes only */
static int image_overlaps(const GfxImageOut* out, u64 gva, u64 size) {
    if (!size) return 0;
    u64 base = out->surf->gva;
    if (gva >= base ? gva - base < out->size : base - gva < size) return 1;
    for (u64 off = 0; off < size;) {
        u64 cpu = 0, n = gmmu_run(gva + off, size - off, &cpu);
        if (!n) {
            u64 left = GMMU_PAGE_SIZE - ((gva + off) & GMMU_PAGE_MASK);
            off += left < size - off ? left : size - off;
            continue;
        }
        if (image_meets(out, cpu, n)) return 1;
        off += n;
    }
    return 0;
}
static void image_remove(size_t index) {
    free(g_image_out.out[index].original);
    free(g_image_out.out[index].runs);
    memmove(g_image_out.out + index, g_image_out.out + index + 1,
            (g_image_out.count - index - 1) * sizeof *g_image_out.out);
    g_image_out.count--;
    g_gfx_img_any = g_image_out.count != 0;
}
/* an entry whose copy's submit is done, landed from the ring */
static void image_land(size_t index) {
    GfxImageOut* o = &g_image_out.out[index];
    vmaInvalidateAllocation(g_vk.vma, o->back_mem, o->back_off, o->back_size);
    vk_surf_flush_land(o->surf, o->back, o->original, o->wgen);
    image_remove(index);
}
/* the entry whose bytes from before the draw are original (each its own allocation): waiting for the GPU can land
   others and move the entries, so an entry is found again after a wait */
static size_t image_find(const u8* original) {
    size_t k = 0;
    while (k < g_image_out.count && g_image_out.out[k].original != original) k++;
    return k;
}
static void image_flush_at(size_t index) {
    GfxImageOut out = g_image_out.out[index];
    SNX_ZONE(z, "vk image flush");
    if (out.sub) { /* its copy is recorded: once its submit is done, from there */
        if (out.sub > vk_last_submit()) vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
        vk_wait_submit(out.sub);
        index = image_find(out.original);
        if (index < g_image_out.count) image_land(index); /* not landed by a frame's landing meanwhile */
        return;
    }
    vk_surf_flush(out.surf, out.original);
    if (out.surf->mem_wgen != out.surf->wgen) fatal("Cannot write back graphics storage image");
    index = image_find(out.original);
    if (index < g_image_out.count) image_remove(index);
}
void vk_gfx_image_mem_need(u64 gva, u64 size) {
    if (!g_gfx_img_any || g_img_landing || !size) return;
    SNX_ZONE(z, "vk image mem need");
    vk_lock();
    g_img_landing = 1;
    for (size_t i = 0; i < g_image_out.count;)
        if (image_overlaps(&g_image_out.out[i], gva, size))
            image_flush_at(i);
        else
            i++;
    g_img_landing = 0;
    vk_unlock();
}
/* every entry holds its surface until it lands, its copy recorded or not (vk_gfx_image_unrecorded leaves those out) */
int vk_gfx_image_holds(const Surf* s) {
    for (size_t i = 0; g_gfx_img_any && i < g_image_out.count; i++)
        if (g_image_out.out[i].surf == s) return 1;
    return 0;
}
int vk_gfx_image_pending(u64 gva, u64 size) {
    int hit = 0;
    if (!g_gfx_img_any || !size) return 0;
    vk_lock();
    for (size_t i = 0; i < g_image_out.count && !hit; i++) hit = image_overlaps(&g_image_out.out[i], gva, size);
    vk_unlock();
    return hit;
}
void vk_gfx_image_land(void) {
    if (!g_gfx_img_any || g_img_landing) return;
    SNX_ZONE(z, "vk image land");
    vk_lock();
    g_img_landing = 1;
    while (g_image_out.count) image_flush_at(0);
    g_img_landing = 0;
    vk_unlock();
}
void vk_gfx_image_put_aside(void) {
    if (!g_gfx_img_any || g_img_landing) return;
    SNX_ZONE(z, "vk image put aside");
    vk_lock();
    g_img_landing = 1;
    vk_cmd(); /* a new frame, if one begins, lands what it lands before the entries are walked */
    for (size_t i = 0; i < g_image_out.count;) {
        GfxImageOut* o = &g_image_out.out[i];
        u64 size, off;
        VkBuffer buf;
        VmaAllocation mem;
        u8* p;
        if (o->sub) {
            i++;
            continue;
        }
        size = vk_surf_flush_size(o->surf);
        p = size ? vk_stage_back(size, &buf, &off, &mem) : NULL;
        if (!p) { /* nothing to copy, or no room in the ring: at once */
            image_flush_at(i);
            continue;
        }
        vk_surf_flush_record(o->surf, buf, off);
        o->sub = vk_submit_number();
        o->back = p;
        o->back_mem = mem;
        o->back_off = off;
        o->back_size = size;
        o->wgen = o->surf->wgen;
        i++;
    }
    g_img_landing = 0;
    vk_unlock();
}
int vk_gfx_image_in_flight(int unsubmitted) {
    for (size_t i = 0; i < g_image_out.count; i++)
        if (g_image_out.out[i].sub && (!unsubmitted || g_image_out.out[i].sub > vk_last_submit())) return 1;
    return 0;
}
void vk_gfx_image_frame_done(u64 sub) {
    int landing = g_img_landing;
    g_img_landing = 1; /* nothing lands the entries over again while they are walked */
    for (size_t i = 0; i < g_image_out.count;)
        if (g_image_out.out[i].sub && g_image_out.out[i].sub <= sub)
            image_land(i);
        else
            i++;
    g_img_landing = landing;
}
