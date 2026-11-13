/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/tex_report.h"
#include "common/utilities/bits.h"
#include "nx/gpu/guest_textures.h"
#include "nx/gpu/vk/range_shape.h"
#include "nx/gpu/vk/tex_census.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"

static struct {
    VkTexEvicted evicted;
    u64 evictions;                            /* the textures the cache evicted */
    u64 reloads, reload_guest, reload_device; /* those made again: their guest bytes, and their images' memory */
    u64 reported;                             /* reloads at the last report */
    /* How long the evicted went unused before they went, and how long after it those made again came back, in
       presents, since the last report: under 60, under 300, under 1200, more. when: the present a key was evicted
       at, by its hash's bits (a key that shares a slot with a later one counts with that one's present) */
    u64 idle[4], back[4];
    u32 when[4096];
} g_tex_report;
static u32 age_class(u32 presents) { return presents < 60 ? 0 : presents < 300 ? 1 : presents < 1200 ? 2 : 3; }

/* what the cache finds t by (texture.c tex_from_tic's search), as one hash */
static u64 tex_key(const Surf* t) {
    VkTexKey k;
    memset(&k, 0, sizeof k); /* its bytes are hashed */
    k.gva = t->gva;
    k.layer_size = t->layer_size;
    k.w = t->w;
    k.h = t->h;
    k.layers = t->layers;
    k.depth = t->depth3d;
    k.levels = t->levels;
    k.type = (u32)t->type;
    k.bl = t->bl;
    k.bh = t->bh;
    k.bd = t->bd;
    k.tile_w = t->tile_w;
    k.pitch = t->pitch;
    k.msaa = t->msaa;
    k.fkey = t->fkey;
    return vk_tex_key_hash(&k);
}

void vk_tex_evicted(const Surf* t) {
    u64 key = tex_key(t);
    g_tex_report.evictions++;
    g_tex_report.idle[age_class(g_vk_present - (u32)t->last_used)]++;
    g_tex_report.when[key >> 20 & 4095] = g_vk_present;
    vk_tex_evicted_add(&g_tex_report.evicted, key);
}

void vk_tex_made(const Surf* t) {
    u64 key = tex_key(t);
    if (!vk_tex_evicted_take(&g_tex_report.evicted, key)) return;
    g_tex_report.back[age_class(g_vk_present - g_tex_report.when[key >> 20 & 4095])]++;
    g_tex_report.reloads++;
    g_tex_report.reload_guest += t->size;
    g_tex_report.reload_device += t->bytes;
}

/* One report's sums over the textures of the cache */
typedef struct {
    /* the game's live texture objects (gpu/guest_textures.h), one block: the copy of them, their ranges joined and
       their first addresses sorted. NULL when the game has none, or with no memory for them */
    GpuTexture* objects;
    VkRange* live;
    u64* starts;
    u32 nstarts, nlive;
    u64 refs; /* the objects, where starts counts their addresses */
    struct {
        u32 n;
        u64 bytes;
    } by_class[VK_TEX_CLASSES];
    int detail; /* buffer_copy_stats: the sums below too */
    VkTexFormats formats;
    VkAstcSizes astc;
    /* the sparse textures: their blocks and mip tails with memory, that memory, and the runs the guest maps under
       them, one block of two arrays of capruns: their GPU addresses, then the guest memory behind each */
    u32 sparse;
    u64 sparse_blocks, sparse_mems, sparse_bytes;
    VkRange* runs;
    u32 nruns, capruns;
    int runs_lost; /* no memory for a run: the pages are not counted */
} Census;

static int is_astc(VkFormat f) { return f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK; }
static int is_bc7(VkFormat f) { return f == VK_FORMAT_BC7_UNORM_BLOCK || f == VK_FORMAT_BC7_SRGB_BLOCK; }

/* the game's live texture objects into c. One made while they are copied can be left out */
static void census_objects(Census* c) {
    const u32 cap = gpu_textures(NULL, 0) + 64;
    void* block = malloc((size_t)cap * (sizeof(GpuTexture) + sizeof(VkRange) + sizeof(u64)));
    if (!block) return;
    c->objects = block;
    c->live = (VkRange*)(void*)(c->objects + cap);
    c->starts = (u64*)(void*)(c->live + cap);
    const u32 had = gpu_textures(c->objects, cap), n = had < cap ? had : cap;
    for (u32 i = 0; i < n; i++) {
        c->live[i] = (VkRange){c->objects[i].gva, c->objects[i].size};
        c->starts[i] = c->objects[i].gva;
        c->refs += c->objects[i].refs;
    }
    qsort(c->starts, n, sizeof *c->starts, snx_compare_u64);
    c->nstarts = n;
    c->nlive = vk_range_join(c->live, n);
}

/* a run the guest maps under a sparse texture (gmmu_pieces), whole pages: n bytes at gva show the guest's from cpu */
static void census_run(void* ctx, u64 gva, u64 cpu, u64 n) {
    Census* c = ctx;
    if (c->runs_lost) return;
    if (c->nruns == c->capruns) {
        const u32 cap = c->capruns ? c->capruns * 2 : 1024;
        VkRange* v = cap > c->capruns ? malloc((size_t)cap * 2 * sizeof *v) : NULL;
        if (!v) {
            c->runs_lost = 1;
            return;
        }
        if (c->nruns) {
            memcpy(v, c->runs, (size_t)c->nruns * sizeof *v);
            memcpy(v + cap, c->runs + c->capruns, (size_t)c->nruns * sizeof *v);
        }
        free(c->runs);
        c->runs = v;
        c->capruns = cap;
    }
    c->runs[c->nruns] = (VkRange){gva, n};
    c->runs[c->capruns + c->nruns++] = (VkRange){cpu, n};
}

static void census_texture(void* ctx, const Surf* t) {
    Census* c = ctx;
    if (c->starts) {
        const int k = vk_tex_live_class(c->starts, c->nstarts, c->live, c->nlive, t->gva);
        c->by_class[k].n++;
        c->by_class[k].bytes += t->bytes;
    }
    if (!c->detail) return;
    /* an ASTC texture the device samples as it is: one row for all its block sizes, which the ASTC sums tell apart */
    const int native = is_astc(t->f.vk);
    vk_tex_formats_add(&c->formats, (u32)(native ? VK_FORMAT_ASTC_4x4_UNORM_BLOCK : t->f.vk), t->bytes);
    const int held = is_bc7(t->f.vk) ? VK_ASTC_AS_BC7 : VK_ASTC_AS_RGBA8; /* one decoded: format.c's TA */
    if (native || t->f.conv == VKC_ASTC)
        vk_astc_sizes_add(&c->astc, t->f.bw, t->f.bh, native ? VK_ASTC_AS_ASTC : held, t->bytes);
    if (!t->sparse && !t->bricks) return;
    c->sparse++;
    c->sparse_blocks += vk_image_bound_blocks(t->img);
    c->sparse_mems += vk_image_bound_memory(t->img);
    c->sparse_bytes += t->bytes;
    gmmu_pieces(t->gva, t->size, census_run, c);
}

/* a host format in a log line: the usual texture formats by name, else Vulkan's number */
static const char* host_format_name(u32 format, char* s, size_t n) {
    static const struct {
        VkFormat f;
        const char* name;
    } names[] = {{VK_FORMAT_R8G8B8A8_UNORM, "RGBA8"},
                 {VK_FORMAT_R8G8B8A8_SRGB, "RGBA8 sRGB"},
                 {VK_FORMAT_B8G8R8A8_UNORM, "BGRA8"},
                 {VK_FORMAT_B8G8R8A8_SRGB, "BGRA8 sRGB"},
                 {VK_FORMAT_R8_UNORM, "R8"},
                 {VK_FORMAT_R8G8_UNORM, "RG8"},
                 {VK_FORMAT_R16_UNORM, "R16"},
                 {VK_FORMAT_R16_SFLOAT, "R16F"},
                 {VK_FORMAT_R16G16_SFLOAT, "RG16F"},
                 {VK_FORMAT_R16G16B16A16_SFLOAT, "RGBA16F"},
                 {VK_FORMAT_R32_SFLOAT, "R32F"},
                 {VK_FORMAT_R32_UINT, "R32UI"},
                 {VK_FORMAT_R32G32B32A32_SFLOAT, "RGBA32F"},
                 {VK_FORMAT_B10G11R11_UFLOAT_PACK32, "R11G11B10F"},
                 {VK_FORMAT_A2B10G10R10_UNORM_PACK32, "RGB10A2"},
                 {VK_FORMAT_BC1_RGB_UNORM_BLOCK, "BC1"},
                 {VK_FORMAT_BC1_RGB_SRGB_BLOCK, "BC1 sRGB"},
                 {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1 with alpha"},
                 {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, "BC1 with alpha sRGB"},
                 {VK_FORMAT_BC2_UNORM_BLOCK, "BC2"},
                 {VK_FORMAT_BC2_SRGB_BLOCK, "BC2 sRGB"},
                 {VK_FORMAT_BC3_UNORM_BLOCK, "BC3"},
                 {VK_FORMAT_BC3_SRGB_BLOCK, "BC3 sRGB"},
                 {VK_FORMAT_BC4_UNORM_BLOCK, "BC4"},
                 {VK_FORMAT_BC4_SNORM_BLOCK, "BC4 signed"},
                 {VK_FORMAT_BC5_UNORM_BLOCK, "BC5"},
                 {VK_FORMAT_BC5_SNORM_BLOCK, "BC5 signed"},
                 {VK_FORMAT_BC6H_UFLOAT_BLOCK, "BC6H"},
                 {VK_FORMAT_BC6H_SFLOAT_BLOCK, "BC6H signed"},
                 {VK_FORMAT_BC7_UNORM_BLOCK, "BC7"},
                 {VK_FORMAT_BC7_SRGB_BLOCK, "BC7 sRGB"},
                 {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, "ASTC"}};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if ((u32)names[i].f == format) return names[i].name;
    snprintf(s, n, "format %u", format);
    return s;
}

/* text added to a line of size bytes at *at: what does not fit is cut */
static void line_add(char* line, size_t size, size_t* at, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
static void line_add(char* line, size_t size, size_t* at, const char* fmt, ...) {
    if (*at >= size) return;
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(line + *at, size - *at, fmt, ap);
    va_end(ap);
    if (w > 0) *at += (size_t)w;
}

#define MB(bytes) ((unsigned long long)((bytes) >> 20))

/* the sums by host format, the most bytes first, and by ASTC block size */
static void formats_log(Census* c) {
    static const char* const as_name[VK_ASTC_AS_N] = {"BC7", "RGBA8", "ASTC"};
    char line[768], name[24];
    size_t at = 0;
    VkTexFormatRow rest = c->formats.other;
    vk_tex_formats_sort(&c->formats);
    for (u32 i = 0; i < c->formats.n; i++) {
        const VkTexFormatRow* r = &c->formats.row[i];
        if (i < 10)
            line_add(line, sizeof line, &at, "%s %s %llu MB (%u)", i ? "," : "",
                     host_format_name(r->format, name, sizeof name), MB(r->bytes), r->count);
        else {
            rest.count += r->count;
            rest.bytes += r->bytes;
        }
    }
    if (rest.count) line_add(line, sizeof line, &at, ", others %llu MB (%u)", MB(rest.bytes), rest.count);
    if (at) LOG("vulkan: cached textures by host format, with the count of each:%s\n", line);
    at = 0;
    for (u32 bw = 0; bw < VK_ASTC_SIDES; bw++)
        for (u32 bh = 0; bh < VK_ASTC_SIDES; bh++) {
            const u64* b = c->astc.bytes[bw][bh];
            const char* join = "";
            if (!b[VK_ASTC_AS_BC7] && !b[VK_ASTC_AS_RGBA8] && !b[VK_ASTC_AS_ASTC]) continue;
            line_add(line, sizeof line, &at, "%s %ux%u", at ? "," : "", bw + 4, bh + 4);
            for (u32 as = 0; as < VK_ASTC_AS_N; as++) {
                if (!b[as]) continue;
                line_add(line, sizeof line, &at, "%s %llu MB as %s", join, MB(b[as]), as_name[as]);
                join = " and";
            }
        }
    if (at) LOG("vulkan: cached ASTC textures by block size:%s\n", line);
}

/* the sparse textures' blocks with memory against the pages the guest maps under them */
static void sparse_log(Census* c) {
    u64 gran[VK_SHAPE_GRANS];
    VkRangeShape gpu = {0}, cpu = {0};
    for (u32 k = 0; k < VK_SHAPE_GRANS; k++) gran[k] = GMMU_PAGE_SIZE;
    if (c->nruns && !c->runs_lost) {
        vk_range_shape(c->runs, c->nruns, gran, &gpu);
        vk_range_shape(c->runs + c->capruns, c->nruns, gran, &cpu);
    }
    if (c->runs_lost)
        LOG("vulkan: sparse textures: %u hold %llu blocks and mip tails with memory, on %llu allocations (%llu MB). "
            "No memory to count the pages the guest maps under them\n",
            c->sparse, (unsigned long long)c->sparse_blocks, (unsigned long long)c->sparse_mems,
            MB(c->sparse_bytes));
    else
        LOG("vulkan: sparse textures: %u hold %llu blocks and mip tails with memory, on %llu allocations (%llu MB). "
            "The guest maps %llu pages of their GPU addresses (%llu different pages) to %llu different pages of its "
            "memory\n",
            c->sparse, (unsigned long long)c->sparse_blocks, (unsigned long long)c->sparse_mems, MB(c->sparse_bytes),
            (unsigned long long)(gpu.sum / GMMU_PAGE_SIZE), (unsigned long long)gpu.granules[0],
            (unsigned long long)cpu.granules[0]);
}

void vk_tex_report_log(void) {
    Census c;
    memset(&c, 0, sizeof c); /* past a kilobyte */
    c.detail = g_settings.buffer_copy_stats;
    /* a game whose front end has no texture objects has no dead textures to tell */
    if (gpu_textures_added()) census_objects(&c);
    if (c.starts || c.detail) vk_surf_each(VK_LRU_TEX, census_texture, &c);
    if (c.starts)
        LOG("vulkan: textures by the game's texture objects: %u at the address of a live object (%llu MB), %u inside "
            "a live object (%llu MB), %u dead with no live object at their address (%llu MB). The game has %llu live "
            "objects at %u addresses\n",
            c.by_class[VK_TEX_LIVE].n, MB(c.by_class[VK_TEX_LIVE].bytes), c.by_class[VK_TEX_INSIDE].n,
            MB(c.by_class[VK_TEX_INSIDE].bytes), c.by_class[VK_TEX_DEAD].n, MB(c.by_class[VK_TEX_DEAD].bytes),
            (unsigned long long)c.refs, c.nstarts);
    if (g_tex_report.evictions)
        LOG("vulkan: %llu textures made again after the cache evicted them (%llu MB of guest memory loaded again, "
            "%llu MB of images), %llu of them since the last report. The cache evicted %llu textures in all\n",
            (unsigned long long)g_tex_report.reloads, MB(g_tex_report.reload_guest), MB(g_tex_report.reload_device),
            (unsigned long long)(g_tex_report.reloads - g_tex_report.reported),
            (unsigned long long)g_tex_report.evictions);
    g_tex_report.reported = g_tex_report.reloads;
    const u64 *idle = g_tex_report.idle, *back = g_tex_report.back;
    if (idle[0] | idle[1] | idle[2] | idle[3] | back[0] | back[1] | back[2] | back[3])
        LOG("vulkan: since the last report the cache evicted %llu textures unused for under 60 presents, %llu for 60 "
            "to 299, %llu for 300 to 1199, %llu for longer; of those made again %llu came back under 60 presents "
            "after they went, %llu after 60 to 299, %llu after 300 to 1199, %llu later\n",
            (unsigned long long)idle[0], (unsigned long long)idle[1], (unsigned long long)idle[2],
            (unsigned long long)idle[3], (unsigned long long)back[0], (unsigned long long)back[1],
            (unsigned long long)back[2], (unsigned long long)back[3]);
    memset(g_tex_report.idle, 0, sizeof g_tex_report.idle);
    memset(g_tex_report.back, 0, sizeof g_tex_report.back);
    if (c.detail) {
        if (c.sparse) sparse_log(&c);
        formats_log(&c);
    }
    free(c.objects);
    free(c.runs);
}
