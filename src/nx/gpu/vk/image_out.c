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
   their memory sooner lands them at once, waiting for that submit when its copy is recorded.
   A kept entry (vk_surf_keep_begin at the bind) holds its span GPU-owned instead of its bytes from before the draw:
   the release publishes its image over the span, and the bytes land when the CPU touches them. Only where the CPU
   touched the span since the bind does it become a write-back, its bytes from before the draw read back from that
   owner (vk_surf_keep_end) */
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
    /* id: its own, never 0. after: the entry whose landed bytes are its bytes from before the draw (original is NULL
       until that one lands), 0 for none. gw: guest memory's write generation when its copy was recorded */
    u64 id, after, gw;
    VkImageKeep keep; /* kept: keep.held, the span's owner from the bind on, whose reference the entry holds */
} GfxImageOut;
static struct {
    GfxImageOut* out;
    size_t count, cap;
    u64 ids;
    /* guest bytes copied on the CPU before draws, bytes copied back from the GPU, images published; and as of the
       last report */
    u64 copied, read_back, published, reported[3];
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
void vk_gfx_image_record(Surf* surf, u8* original, u32 size, u64 after, const VkImageKeep* keep) {
    VkImageKeep k = keep ? *keep : (VkImageKeep){0};
    if (original || k.made) g_image_out.copied += size; /* read by the draw's setup, or staged by its bind */
    if (vk_gfx_image_unrecorded(surf)) {
        free(original);
        if (k.held) vk_own_res_unref(k.held);
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
    g_image_out.out[g_image_out.count++] = (GfxImageOut){.surf = surf,
                                                         .original = original,
                                                         .size = size,
                                                         .runs = runs,
                                                         .nruns = nruns,
                                                         .id = ++g_image_out.ids,
                                                         .after = after,
                                                         .keep = k};
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
    if (g_image_out.out[index].keep.held) vk_own_res_unref(g_image_out.out[index].keep.held);
    free(g_image_out.out[index].original);
    free(g_image_out.out[index].runs);
    memmove(g_image_out.out + index, g_image_out.out + index + 1,
            (g_image_out.count - index - 1) * sizeof *g_image_out.out);
    g_image_out.count--;
    g_gfx_img_any = g_image_out.count != 0;
}
/* the entry of id: waiting for the GPU can land others and move the entries, so an entry is found again after one */
static size_t image_find(u64 id) {
    size_t k = 0;
    while (k < g_image_out.count && g_image_out.out[k].id != id) k++;
    return k;
}
/* the entries that go on from id get their bytes from before their draws: bytes (bytes_size of them, the landing's,
   handed to the first) or, without them, guest memory, which the landing of id has just written */
static void image_follow(u64 id, u8* bytes, u64 bytes_size) {
    for (size_t j = 0; j < g_image_out.count; j++) {
        GfxImageOut* f = &g_image_out.out[j];
        if (f->after != id) continue;
        f->after = 0;
        if (bytes && bytes_size == f->size) {
            f->original = bytes;
            bytes = NULL;
        } else {
            f->original = malloc(f->size);
            if (!f->original) fatal("Out of memory recording graphics storage image");
            gmem_read(f->surf->gva, f->original, f->size);
            g_image_out.copied += f->size;
        }
    }
    free(bytes);
}
static int image_followed(u64 id) {
    for (size_t j = 0; j < g_image_out.count; j++)
        if (g_image_out.out[j].after == id) return 1;
    return 0;
}
static void image_land(size_t index);
/* the entry it goes on from lands first (its copy is in the same submit or an earlier one), which gives it its bytes */
static size_t image_ready(size_t index) {
    u64 id = g_image_out.out[index].id, after = g_image_out.out[index].after;
    if (after) {
        size_t k = image_find(after);
        if (k < g_image_out.count) image_land(k);
        index = image_find(id);
        if (index < g_image_out.count && g_image_out.out[index].after)
            image_follow(after, NULL, 0); /* gone without landing */
    }
    return index;
}
/* an entry whose copy's submit is done, landed from the ring */
static void image_land(size_t index) {
    if ((index = image_ready(index)) >= g_image_out.count) return;
    GfxImageOut* o = &g_image_out.out[index];
    u64 id = o->id, keep_size = 0;
    u8* keep = NULL;
    vmaInvalidateAllocation(g_vk.vma, o->back_mem, o->back_off, o->back_size);
    vk_surf_flush_land(o->surf, o->back, o->original, o->wgen, image_followed(id) ? &keep : NULL, &keep_size);
    image_follow(id, keep, keep_size);
    if ((index = image_find(id)) < g_image_out.count) image_remove(index);
}
/* An entry a draw's writes to surf go on from instead of reading its bytes from before the draw now: the newest over
   surf's bytes, when it is surf's own, the same size, its copy recorded (sub), and guest memory unwritten there since.
   Its landed bytes are then this draw's from before it. Reading guest memory here landed it, a wait for the GPU: 6
   times a frame in one game's scene. 0 when another entry is over those bytes, or none is */
u64 vk_gfx_image_after(Surf* surf, u32 size) {
    u64 id = 0;
    for (size_t i = 0; i < g_image_out.count; i++) {
        const GfxImageOut* o = &g_image_out.out[i];
        if (!image_overlaps(o, surf->gva, size)) continue;
        if (o->surf != surf || !o->sub || o->size != size || gpu_mem_dirty_since(o->gw, surf->gva, size)) return 0;
        id = o->id;
    }
    return id;
}
/* a kept entry ended (vk_surf_keep_end): published and gone, 1; or a write-back from now on, the CPU having touched
   its span, with its bytes from before the draw read back (or none, where they couldn't be), 0 */
static int image_keep_end(u64 id) {
    size_t index = image_find(id);
    GfxImageOut* o = &g_image_out.out[index];
    VkImageKeep k = o->keep;
    u64 size = o->size;
    u8* original;
    o->keep.held = NULL; /* vk_surf_keep_end takes the reference */
    if (vk_surf_keep_end(o->surf, &k, &original)) {
        g_image_out.published++;
        if ((index = image_find(id)) < g_image_out.count) image_remove(index);
        return 1;
    }
    if (original) g_image_out.read_back += size;
    if ((index = image_find(id)) >= g_image_out.count) { /* the wait for that read landed it: nothing is left */
        free(original);
        return 1;
    }
    g_image_out.out[index].original = original;
    return 0;
}
static void image_flush_at(size_t index) {
    GfxImageOut out = g_image_out.out[index];
    SNX_ZONE(z, "vk image flush");
    if (out.keep.held) { /* published (what needs its bytes lands them from there), or merged at once */
        if (image_keep_end(out.id)) return;
        out = g_image_out.out[index = image_find(out.id)];
    }
    if (out.sub) { /* its copy is recorded: once its submit is done, from there */
        if (out.sub > vk_last_submit()) vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
        vk_wait_submit(out.sub);
        index = image_find(out.id);
        if (index < g_image_out.count) image_land(index); /* not landed by a frame's landing meanwhile */
        return;
    }
    if (out.after) { /* the entry it goes on from lands first, waiting for its copy, and gives it its bytes */
        size_t k = image_find(out.after);
        if (k < g_image_out.count) image_flush_at(k);
        if ((index = image_ready(image_find(out.id))) >= g_image_out.count) return;
        out = g_image_out.out[index];
    }
    g_image_out.read_back += vk_surf_flush_size(out.surf);
    vk_surf_flush(out.surf, out.original);
    if (out.surf->mem_wgen != out.surf->wgen) fatal("Cannot write back graphics storage image");
    index = image_find(out.id);
    if (index < g_image_out.count) image_remove(index);
}
void vk_gfx_image_mem_need(u64 gva, u64 size) {
    if (!g_gfx_img_any || g_img_landing || !size) return;
    SNX_ZONE(z, "vk image mem need");
    vk_lock();
    g_img_landing = 1;
    /* a landing can take entries before i with it (the one an entry goes on from): the walk starts over */
    for (size_t i = 0; i < g_image_out.count;)
        if (image_overlaps(&g_image_out.out[i], gva, size)) {
            image_flush_at(i);
            i = 0;
        } else
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
        if (o->keep.held) { /* published, or a write-back from here on */
            u64 id = o->id;
            if (image_keep_end(id)) continue;
            o = &g_image_out.out[i = image_find(id)];
            if (!o->original) { /* its bytes from before the draw couldn't be read back */
                image_flush_at(i);
                continue;
            }
        }
        size = vk_surf_flush_size(o->surf);
        p = size ? vk_stage_back(size, &buf, &off, &mem) : NULL;
        if (!p) { /* nothing to copy, or no room in the ring: at once */
            image_flush_at(i);
            continue;
        }
        vk_surf_flush_record(o->surf, buf, off);
        g_image_out.read_back += size;
        o->sub = vk_submit_number();
        o->back = p;
        o->back_mem = mem;
        o->back_off = off;
        o->back_size = size;
        o->wgen = o->surf->wgen;
        o->gw = gpu_mem_generation();
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
void vk_gfx_image_stats(u64* copied, u64* read_back, u64* published) {
    *copied = g_image_out.copied;
    *read_back = g_image_out.read_back;
    *published = g_image_out.published;
}
void vk_gfx_image_report(void) {
    u64* was = g_image_out.reported;
    LOG("vulkan: draws' storage images since the last report: %llu bytes copied before their draws, %llu copied back, "
        "%llu images published\n",
        (unsigned long long)(g_image_out.copied - was[0]), (unsigned long long)(g_image_out.read_back - was[1]),
        (unsigned long long)(g_image_out.published - was[2]));
    was[0] = g_image_out.copied;
    was[1] = g_image_out.read_back;
    was[2] = g_image_out.published;
}
