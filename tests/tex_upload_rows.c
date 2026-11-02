/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* tex_upload (vk/texture.c) from guest memory through the staging buffer and the GPU's deswizzles and conversions
   (a CPU model of them below, run as they are recorded, which checks the barriers around them) to the copies into
   the image, each copy's bytes checked against a walk of the block-linear layout byte by byte (bl_offset below): a
   level whose rows are whole 16-byte pieces, one whose rows end inside a piece, a 3D level with blocks of slices, mip
   levels, a level read through gmem_read, a pitch-linear level, a converted format, and a sparse texture's levels
   whose rows are whole tiles. No level takes a buffer of its size on the CPU. Levels past the allocation cap go in
   strips, each region checked where it lands, and an ASTC level's block rows reach vk_astc_upload strip by strip,
   deswizzled */
#include <stdlib.h>
#include <string.h>
static size_t g_watch, g_big; /* allocations of g_watch bytes or more, while watched */
static void* counted_malloc(size_t n) {
    if (g_watch && n >= g_watch) g_big++;
    return malloc(n);
}
#define malloc counted_malloc
#pragma GCC diagnostic ignored "-Wmissing-field-initializers" /* texture.c builds without -Wextra */
#include "nx/gpu/vk/texture.c"
#undef malloc
#include "nx/hle/services/nvdrv.h" /* gmmu_pieces, and g_mem, which the stubs below stand in for */
#include "nx/memory/mem.h"

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ------------------------------------------------ guest memory and the staging ring */
#define GVA   0x100000ull
#define GUEST (4u << 20)
static u8 guest[GUEST];
static u64 guest_end = GVA + GUEST; /* gmem_ptr's view ends here: a level past it reads through gmem_read */
u8* gmem_ptr(u64 gva, u64 size) {
    if (gva < GVA || gva + size > guest_end) return NULL;
    return guest + (gva - GVA);
}
int gmem_read(u64 gva, void* dst, u64 size) {
    for (u64 i = 0; i < size; i++) ((u8*)dst)[i] = gva + i >= GVA && gva + i < guest_end ? guest[gva + i - GVA] : 0;
    return 0;
}
/* nothing is GPU-owned here: vk_guest_read reads guest memory as gmem_read does, into staging */
int g_own_any;
void vk_guest_read(u64 gva, u64 size, u8* host, VkBuffer buf, u64 at) {
    (void)buf, (void)at;
    gmem_read(gva, host, size);
}
void vk_guest_peek(u64 gva, void* dst, u64 size) { gmem_read(gva, dst, size); }
/* a texture with holes in its mapping (a virtual pool's, mapped a tile at a time): the runs g_map lists are what the
   GPU's page table maps, guest's bytes at the same offset (GP(cpu) is guest + cpu), and no texture is one host run */
u8* g_mem = guest;
static struct {
    u64 gva[8], n[8];
    u32 count;
} g_map;
void gmmu_pieces(u64 gva, u64 size, void (*fn)(void* ctx, u64 gva, u64 cpu, u64 n), void* ctx) {
    for (u32 i = 0; i < g_map.count; i++)
        if (g_map.gva[i] < gva + size && g_map.gva[i] + g_map.n[i] > gva)
            fn(ctx, g_map.gva[i], g_map.gva[i] - GVA, g_map.n[i]);
}
u8* gmem_host(u64 gva, u64 size) { return g_map.count ? NULL : gmem_ptr(gva, size); }
#define STAGE (8u << 20)
#define STAGE_BUF ((VkBuffer)(uintptr_t)0x5157)
#define SCRATCH_BUF ((VkBuffer)(uintptr_t)0x5C2A)
/* where the GPU's batch is (vk_bl_barrier below): 1 after the barrier before its dispatches, 2 after one after them;
   batch_moves since 1 */
static u32 bar_state, batch_moves;
static u8 stage[STAGE];
static u64 stage_used;
static u32 copy_bpp = 4, copy_bw = 1, copy_bh = 1; /* the texture's: the bytes of a copy's texel or block */
u8* vk_stage(u64 size, u64 align, VkBuffer* buf, u64* off) {
    u64 at = (stage_used + align - 1) & ~(align - 1);
    if (at + size > STAGE) return NULL;
    memset(stage + at, 0xA5, size); /* bytes the upload leaves alone show */
    stage_used = at + size;
    *buf = STAGE_BUF;
    *off = at;
    return stage + at;
}
u64 vk_stage_align(void) { return 64; }
/* the scratch buffer the GPU deswizzles and converts into, which a CPU model of those dispatches writes here */
#define SCRATCH (8u << 20)
static u8 scratch[SCRATCH];
static u8* mem_of(VkBuffer b) { return b == STAGE_BUF ? stage : b == SCRATCH_BUF ? scratch : NULL; }
/* the copies into the image, with the bytes each copies taken when it is recorded (the scratch buffer is reused):
   a region's bufferOffset is where its bytes start in copied */
static VkBufferImageCopy regs[1024];
static u32 nregs;
static u8 copied[16u << 20];
static u64 copied_used;
static void VKAPI_CALL copy_to_image(VkCommandBuffer cb, VkBuffer b, VkImage img, VkImageLayout l, uint32_t n,
                                     const VkBufferImageCopy* r) {
    (void)cb, (void)img, (void)l;
    for (u32 i = 0; i < n && nregs < 1024; i++) {
        VkBufferImageCopy c = r[i];
        u32 bpp = c.imageSubresource.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT ? 1 : copy_bpp;
        u64 row = (u64)(c.bufferRowLength ? c.bufferRowLength : c.imageExtent.width) / copy_bw * bpp;
        u64 rows = (c.bufferImageHeight ? c.bufferImageHeight : c.imageExtent.height) / copy_bh;
        u64 size = row * rows * c.imageExtent.depth;
        CHECK(copied_used + size <= sizeof copied && mem_of(b) && (b != SCRATCH_BUF || bar_state == 2));
        memcpy(copied + copied_used, mem_of(b) + c.bufferOffset, size);
        c.bufferOffset = copied_used;
        copied_used += size;
        regs[nregs++] = c;
    }
}
PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage = copy_to_image;
PFN_vkCmdBlitImage vkCmdBlitImage;
PFN_vkCmdClearColorImage vkCmdClearColorImage;
PFN_vkCmdClearDepthStencilImage vkCmdClearDepthStencilImage;
PFN_vkCmdCopyImage vkCmdCopyImage;
PFN_vkCreateImageView vkCreateImageView;
PFN_vkCreateSampler vkCreateSampler;
PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties;
/* record.c's wrappers, which the frame's command buffer goes through: made at once here */
void rec_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout dl, u32 n,
                              const VkBufferImageCopy* r) {
    vkCmdCopyBufferToImage(cb, src, dst, dl, n, r);
}
void rec_CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, u32 n,
                      const VkImageCopy* r) {
    vkCmdCopyImage(cb, src, sl, dst, dl, n, r);
}
void rec_CmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, u32 n,
                      const VkImageBlit* r, VkFilter filter) {
    vkCmdBlitImage(cb, src, sl, dst, dl, n, r, filter);
}
void rec_CmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearColorValue* c, u32 n,
                            const VkImageSubresourceRange* r) {
    vkCmdClearColorImage(cb, img, l, c, n, r);
}
void rec_CmdClearDepthStencilImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearDepthStencilValue* c,
                                   u32 n, const VkImageSubresourceRange* r) {
    vkCmdClearDepthStencilImage(cb, img, l, c, n, r);
}

/* ------------------------------------------------ what texture.c reaches otherwise, unused here */
VkState g_vk;
_Atomic int g_cs_any;
u32 g_vk_present;
void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }
int vk_ok_(VkResult r, const char* what, const char* file, int line) {
    (void)what, (void)file, (void)line;
    return r == VK_SUCCESS;
}
VkCommandBuffer vk_cmd(void) { return (VkCommandBuffer)(uintptr_t)0xCB; }
void vk_render_end_at(const char* file, int line) {
    (void)file;
    (void)line;
}
void vk_surf_use(VkCommandBuffer cb, Surf* s, VkImageLayout layout, VkPipelineStageFlags2 stage_,
                 VkAccessFlags2 access) {
    (void)cb, (void)s, (void)layout, (void)stage_, (void)access;
}
VkImageAspectFlags vk_aspect(const VkFmt* f) {
    return f->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}
/* the ASTC upload asks for its block rows in strips of 3: they land in astc_out, slice after slice */
static u8 astc_out[128 << 10];
int vk_astc_upload(VkCommandBuffer cb, Surf* t, u32 w, u32 h, u32 depth, u32 level, u32 layer, VkAstcRows rows,
                   void* ctx) {
    (void)level, (void)layer;
    u32 nx = (w + t->f.bw - 1) / t->f.bw, ny = (h + t->f.bh - 1) / t->f.bh;
    for (u32 z = 0; z < depth; z++)
        for (u32 y = 0; y < ny; y += 3) {
            u32 n = ny - y < 3 ? ny - y : 3;
            VkBuffer b;
            u64 off;
            CHECK(rows(ctx, &cb, z, 1, y, n, &b, &off) && mem_of(b));
            memcpy(astc_out + ((u64)z * ny + y) * nx * 16, mem_of(b) + off, (u64)n * nx * 16);
        }
    return 1;
}
/* a CPU model of the GPU's moves (vk/blocklinear.c) and conversions (vk/texconv.c), done as they are recorded: the
   recorded order is the order the barriers keep. Conversions: RGBA4 only, the format the cases convert */
/* static u32 moves */
static u32 convs;
void vk_bl_barrier(VkCommandBuffer cb, int before) {
    (void)cb;
    bar_state = before ? 1 : 2;
    if (before) batch_moves = 0;
}
VkBuffer vk_bl_scratch(u64 size, u64* cap) {
    *cap = SCRATCH < g_vk.alloc_cap ? SCRATCH : g_vk.alloc_cap;
    return size <= *cap ? SCRATCH_BUF : VK_NULL_HANDLE;
}
/* the GOBs across a move's rows of blocks: its own, or its row's */
static u32 move_gobs(const VkBlMove* m) { return m->gobs_x ? m->gobs_x : (m->row_bytes + 63) >> 6; }
static u64 bl_offset_g(u32 x, u32 y, u32 z, u32 gob_w, u32 h, u32 bh_log2, u32 bd_log2);
void vk_bl_range(const VkBlMove* m, u64* lo, u64* hi) {
    u64 block_row = (u64)move_gobs(m) << (9 + m->bh + m->bd);
    u64 slab = (((((u64)m->height + 7) >> 3) + (1u << m->bh) - 1) >> m->bh) * block_row;
    u32 last = m->y0 + m->rows - 1, zl = m->z0 + m->slices - 1;
    *lo = (u64)(m->z0 >> m->bd) * slab + (u64)((m->y0 >> 3) >> m->bh) * block_row;
    *hi = (u64)(zl >> m->bd) * slab + (u64)(((last >> 3) >> m->bh) + 1) * block_row;
}
int vk_bl_move(VkCommandBuffer cb, const VkBlMove* m, int to_bl, VkBuffer bl, u64 bl_at, VkBuffer lin, u64 lin_at) {
    (void)cb;
    u64 lo, hi;
    vk_bl_range(m, &lo, &hi);
    CHECK(!to_bl && mem_of(bl) && mem_of(lin) && bar_state == 1 && m->lin_pitch % 16 == 0 &&
          m->lin_pitch >= ((m->row_bytes + 15) & ~15u));
    for (u32 z = 0; z < m->slices; z++)
        for (u32 y = 0; y < m->rows; y++)
            for (u32 x = 0; x < m->row_bytes; x++) {
                u64 o = bl_offset_g(x, m->y0 + y, m->z0 + z, move_gobs(m), m->height, m->bh, m->bd);
                CHECK(o >= lo && o < hi);
                mem_of(lin)[lin_at + ((u64)z * m->rows + y) * m->lin_pitch + x] = mem_of(bl)[bl_at + o - lo];
            }
    /*moves++;*/
    batch_moves++;
    return 1;
}
int vk_tc_converts(const VkFmt* f) { return f->conv != VKC_NONE && f->conv != VKC_ASTC; }
VkTcShape vk_tc_shape(const VkFmt* f, u32 w) {
    CHECK(f->conv == VKC_RGBA4);
    return (VkTcShape){.units = (w + 1) / 2, .out_texels = (w + 1) / 2 * 2, .out_bpp = 4, .out_rows = 1, .in_align = 16};
}
u64 vk_tc_out_size(const VkTcShape* s, u32 rows, u32 slices) { return (u64)s->out_texels * 4 * rows * slices; }
int vk_tc_convert(VkCommandBuffer cb, const VkFmt* f, u32 w, u32 rows, u32 slices, VkBuffer in, u64 in_at,
                  u32 in_pitch, VkBuffer out, u64 out_at) {
    (void)cb, (void)f;
    u32 ow = (w + 1) / 2 * 2;
    CHECK(mem_of(in) && mem_of(out) && (bar_state == 2 || (bar_state == 1 && !batch_moves)));
    for (u32 y = 0; y < rows * slices; y++)
        for (u32 x = 0; x < w; x++) {
            const u8* t = mem_of(in) + in_at + (u64)y * in_pitch + x * 2;
            u32 v = t[0] | t[1] << 8;
            u32 c = (v & 15) * 17 | ((v >> 4) & 15) * 17 << 8 | ((v >> 8) & 15) * 17 << 16 | (v >> 12) * 17 << 24;
            memcpy(mem_of(out) + out_at + ((u64)y * ow + x) * 4, &c, 4);
        }
    convs++;
    return 1;
}
void vk_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkPipelineStageFlags2 src_stage,
                VkAccessFlags2 src_access, VkImageLayout from, VkPipelineStageFlags2 dst_stage,
                VkAccessFlags2 dst_access, VkImageLayout to) {
    (void)cb, (void)img, (void)aspect, (void)src_stage, (void)src_access, (void)from, (void)dst_stage, (void)dst_access,
        (void)to;
}
void vk_compute_mem_need(u64 a, u64 n) { (void)a, (void)n; }
void vk_defer_image(VkImage img, VmaAllocation a, VkImageView v1, VkImageView v2) {
    (void)img, (void)a, (void)v1, (void)v2;
}
u64 vk_hash(const void* p, u64 size) { /* FNV-1a over the bytes: mem_hash's check below tells contents apart */
    const u8* b = (const u8*)p;
    u64 h = 1469598103934665603ull;
    for (u64 i = 0; i < size; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
int vk_rt_drawn_fresh(Surf* s) {
    (void)s;
    return 0;
}
int vk_rt_drawn_in(u64 lo, u64 hi, Surf** out, int max) {
    (void)lo, (void)hi, (void)out, (void)max;
    return 0;
}
Surf* vk_rt_find(u64 gva) {
    (void)gva;
    return NULL;
}
void vk_rt_note_read(Surf* s) { (void)s; }
u64 vk_sample_hash(u64 gva, u64 len) {
    (void)gva;
    return len;
}
u32 vk_sc(u32 v) { return v ? v : 1; }
VkFormat vk_srgb_as(VkFormat f, int srgb) {
    (void)srgb;
    return f;
}
void vk_surf_add(Surf* s) { (void)s; }
void vk_surf_touch(Surf* s) { (void)s; }
void vk_surf_relist(Surf* s) { (void)s; }
u32 vk_surf_gen(void) {
    static u32 gen;
    return ++gen;
}
u64 vk_surf_reclaim(int list, u32 before, u64 want, int lossy) {
    (void)list, (void)before, (void)want, (void)lossy;
    return 0;
}
u64 vk_rt_reclaim_superseded(u32 before, u64 want) {
    (void)before, (void)want;
    return 0;
}
u32 vk_present_before(u64 ago_ns, u32 presents) {
    (void)ago_ns, (void)presents;
    return 0;
}
void vk_defer_bytes(u64 bytes) { (void)bytes; }
Surf* vk_surf_first(u64 gva) {
    (void)gva;
    return NULL;
}
VkFmt vk_tic_format(u32 w0, int srgb) {
    (void)w0, (void)srgb;
    return (VkFmt){0};
}
VkFmt vk_tic_format_3d(u32 w0, int srgb) { return vk_tic_format(w0, srgb); }
VkImage vk_unscaled_image(const Surf* s, u32 layers, VmaAllocation* mem) {
    (void)s, (void)layers, (void)mem;
    return VK_NULL_HANDLE;
}
void vk_view_formats(VkImageCreateInfo* ii, VkViewFormats* vf, VkFormat raw) { (void)ii, (void)vf, (void)raw; }
/* multisampled surfaces: none here */
VkFormat vk_raw_format(u32 bpp) {
    (void)bpp;
    return VK_FORMAT_UNDEFINED;
}
int vk_ms_supported(VkFormat f, VkImageUsageFlags usage, VkImageCreateFlags flags, u32 msaa) {
    (void)f, (void)usage, (void)flags, (void)msaa;
    return 0;
}
int vk_ms_moves(const Surf* s) {
    (void)s;
    return 0;
}
int vk_ms_load(Surf* s) {
    (void)s;
    return 0;
}
VkImage vk_ms_unpack(VkCommandBuffer cb, Surf* s, u32 layer) {
    (void)cb, (void)s, (void)layer;
    return VK_NULL_HANDLE;
}
/* write tracking: the stamp is on guest's bytes [at, at + len), all of them when len is 0. As buffer.c's, it answers
   only for a host range */
static struct {
    int on;
    u32 clock, stamp, epoch;
    u64 at, len;
} g_tracking;
static int stamped(u64 o, u64 n, u32 since) {
    return g_tracking.stamp > since &&
           (!g_tracking.len || (o < g_tracking.at + g_tracking.len && g_tracking.at < o + n));
}
int vk_written_since(const u8* host, u64 len, u32 since) {
    if (!g_tracking.on || !host || !len) return -1;
    return stamped((u64)(host - guest), len, since);
}
u32 vk_ww_now(void) { return g_tracking.clock; }
u32 vk_ww_epoch(void) { return g_tracking.epoch; }
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz) {
    (void)since, (void)a, (void)sz;
    return 0;
}
u64 gpu_mem_generation(void) { return 0; }
VkResult vmaCreateImage(VmaAllocator a, const VkImageCreateInfo* ii, const VmaAllocationCreateInfo* ai, VkImage* img,
                        VmaAllocation* mem, VmaAllocationInfo* info) {
    (void)a, (void)ii, (void)ai, (void)img, (void)mem, (void)info;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}
int vk_cache_image_create(const VkImageCreateInfo* ii, VkImage* img, VmaAllocation* mem, u64* bytes) {
    (void)ii, (void)img, (void)mem, (void)bytes;
    return 0;
}
/* images with memory only where it is bound: none here */
int vk_resident_block(VkFormat f, VkImageUsageFlags usage, VkExtent3D* block) {
    (void)f, (void)usage, (void)block;
    return 0;
}
int vk_image_create_resident(const VkImageCreateInfo* ii, const VkExtent3D* block, VkImage* img) {
    (void)ii, (void)block, (void)img;
    return 0;
}
int vk_image_resident(VkImage img, const u8* want, u32 n, u64* bytes) {
    (void)img, (void)want, (void)n, (void)bytes;
    return 0;
}
void vk_surf_bytes(Surf* s, u64 bytes) { s->bytes = bytes; }
void vk_mem_trim(u64 extra, int force) { (void)extra, (void)force; }
/* the block linear layout: GOBs of 64 bytes by 8 rows in blocks of 1 << bh by 1 << bd GOBs, gob_w of them across a
   row of blocks, the GOB's 16 byte groups in their order */
static u64 bl_offset_g(u32 x, u32 y, u32 z, u32 gob_w, u32 h, u32 bh_log2, u32 bd_log2) {
    u32 bh = 1u << bh_log2, bd = 1u << bd_log2;
    u32 blocks_h = ((h + 7) / 8 + bh - 1) / bh, gy = y / 8, xb = x % 64, yb = y % 8;
    u64 block = 512ull * bh * bd;
    u64 o = ((u64)(z / bd) * blocks_h * gob_w + (u64)(gy / bh) * gob_w + x / 64) * block +
            ((u64)(z % bd) * bh + gy % bh) * 512;
    return o + (xb / 32) * 256 + (yb / 2) * 64 + ((xb % 32) / 16) * 32 + (yb % 2) * 16 + xb % 16;
}
/* a row wb bytes wide: as many GOBs as it takes */
u64 bl_offset(u32 x, u32 y, u32 z, u32 wb, u32 h, u32 bh_log2, u32 bd_log2) {
    return bl_offset_g(x, y, z, (wb + 63) / 64, h, bh_log2, bd_log2);
}
/* the GOBs across a level's rows, rowb bytes wide, rows high (of texels or blocks) and ld slices deep: a sparse
   texture's (tile_w) whole tiles when the level is at least a tile wide, high and deep, the tile 2^bh GOBs high and
   2^bd deep, the texture's block (the hardware's TILE_WIDTH_IN_GOBS) */
static u32 level_gobs(const Surf* t, u32 rowb, u32 rows, u32 ld) {
    u32 g = (rowb + 63) / 64, n = 1u << t->tile_w;
    if (t->tile_w && rowb >= 64u << t->tile_w && rows >= 8u << t->bh && ld >= 1u << t->bd) g = (g + n - 1) / n * n;
    return g;
}

/* ------------------------------------------------ the cases */
static void fill(void) {
    for (u32 i = 0; i < GUEST; i++) guest[i] = (u8)(i * 7 + (i >> 9) * 13 + 1);
}
static Surf tex(u32 w, u32 h, u32 d3, u32 levels, u32 bpp, u32 bh, u32 bd, int bl) {
    Surf t;
    memset(&t, 0, sizeof t);
    t.gva = GVA;
    t.w = w;
    t.h = h;
    t.layers = 1;
    t.levels = levels;
    t.depth3d = d3;
    t.type = d3 > 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    t.f = (VkFmt){.vk = VK_FORMAT_R8G8B8A8_UNORM, .bpp = (u8)bpp, .bw = 1, .bh = 1, .conv = VKC_NONE};
    t.bl = (u32)bl;
    t.bh = bh;
    t.bd = bd;
    t.pitch = bl ? 0 : (w * bpp + 31) & ~31u;
    t.img = (VkImage)(uintptr_t)0x1A6;
    return t;
}
/* the upload, then every region against the layout walked byte by byte where it lands: level l's bytes start after
   the levels before it, as tex_upload lays them out, and its regions cover each of its rows once. bands: the regions
   the base level takes, more than one when it goes in bands */
static void upload_and_check(Surf* t, u64 limit, u32 bands) {
    guest_end = GVA + limit;
    stage_used = copied_used = 0;
    nregs = 0;
    g_watch = 0;
    copy_bpp = t->f.bpp;
    copy_bw = t->f.bw;
    copy_bh = t->f.bh;
    CHECK(tex_upload(t));
    guest_end = GVA + GUEST;
    u32 d3 = t->type == VK_IMAGE_VIEW_TYPE_3D ? t->depth3d : 1;
    u64 at = 0;
    u32 first = 0;
    for (u32 l = 0; l < t->levels; l++) {
        u32 lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1, ld = d3 >> l ? d3 >> l : 1;
        u32 rowb = lw * t->f.bpp, lbh = 0, lbd = 0, covered = 0, n = 0, gw = level_gobs(t, rowb, lh, ld);
        u64 lsz = t->bl ? bl_level_size(t->w, t->h, d3, &t->f, t->bh, t->bd, t->tile_w, l, &lbh, &lbd)
                        : (u64)t->pitch * lh * ld;
        /* the level's size: its rows of blocks, gw GOBs across each, in slabs of 2^lbd slices */
        CHECK(!t->bl || lsz == (u64)gw * ((((lh + 7) / 8) + (1u << lbh) - 1) >> lbh) * ((ld + (1u << lbd) - 1) >> lbd) *
                                   (512ull << (lbh + lbd)));
        for (; first + n < nregs && regs[first + n].imageSubresource.mipLevel == l; n++) {
            const VkBufferImageCopy* r = &regs[first + n];
            u32 y0 = (u32)r->imageOffset.y, z0 = (u32)r->imageOffset.z, rows = r->imageExtent.height;
            CHECK(r->imageOffset.x == 0 && r->imageExtent.width == lw && y0 + rows <= lh &&
                  z0 + r->imageExtent.depth <= ld);
            covered += rows * r->imageExtent.depth;
            const u8* s = copied + r->bufferOffset;
            u64 pitch = r->bufferRowLength ? (u64)r->bufferRowLength * t->f.bpp : rowb;
            u64 height = r->bufferImageHeight ? r->bufferImageHeight : rows;
            CHECK(pitch >= rowb && height >= rows);
            for (u32 z = z0; z < z0 + r->imageExtent.depth; z++)
                for (u32 y = y0; y < y0 + rows; y++)
                    for (u32 x = 0; x < rowb; x++) {
                        u64 o = t->bl ? bl_offset_g(x, y, z, gw, lh, lbh, lbd) : (u64)(z * lh + y) * t->pitch + x;
                        u8 want = at + o < limit ? guest[at + o] : 0;
                        CHECK(s[((u64)(z - z0) * height + (y - y0)) * pitch + x] == want);
                    }
        }
        CHECK(covered == lh * ld && (l || n == bands));
        first += n;
        at += lsz;
    }
    CHECK(first == nregs);
}

/* the 4 bytes a texel of level l at (x, y, z) was copied as, by the last region recorded over it; NULL when none was */
static const u8* copied_texel(u32 l, u32 x, u32 y, u32 z) {
    const u8* got = NULL;
    for (u32 i = 0; i < nregs; i++) {
        const VkBufferImageCopy* r = &regs[i];
        u32 x0 = (u32)r->imageOffset.x, y0 = (u32)r->imageOffset.y, z0 = (u32)r->imageOffset.z;
        if (r->imageSubresource.mipLevel == l && x - x0 < r->imageExtent.width && y - y0 < r->imageExtent.height &&
            z - z0 < r->imageExtent.depth)
            got = copied + r->bufferOffset +
                  (((u64)(z - z0) * r->bufferImageHeight + (y - y0)) * r->bufferRowLength + (x - x0)) * 4;
    }
    return got;
}
/* the RGBA8 texel a texture of 4 byte texels has at (x, y, z) of a level at at in guest, its rows rowb bytes */
static u32 guest_texel(u64 at, u32 x, u32 y, u32 z, u32 rowb, u32 lh, u32 lbh, u32 lbd) {
    u32 v = 0;
    for (u32 k = 0; k < 4; k++) v |= (u32)guest[at + bl_offset(x * 4 + k, y, z, rowb, lh, lbh, lbd)] << (8 * k);
    return v;
}
/* a volume in a virtual pool's 64 KiB pages: RGBA8, 208x64x32 (13 GOBs a row) in blocks 1 GOB high and 16 deep, levels
   levels; pages 0 and 2 mapped, and its checks settled. Its size */
static Surf sparse_volume(u32 levels) {
    Surf t = tex(208, 64, 32, levels, 4, 0, 4, 1);
    for (u32 l = 0; l < levels; l++) t.size += bl_level_size(t.w, t.h, 32, &t.f, 0, 4, 0, l, NULL, NULL);
    g_map.count = 2;
    g_map.gva[0] = GVA, g_map.n[0] = 0x10000;
    g_map.gva[1] = GVA + 0x20000, g_map.n[1] = 0x10000;
    copy_bpp = 4, copy_bw = copy_bh = 1;
    t.upload_valid = 1;
    t.hash = mem_hash(t.gva, t.size);
    t.stable = 100;
    t.shash = vk_sample_hash(t.gva, t.size);
    t.tfull = t.checked = g_vk_present;
    t.next_check = g_vk_present + 1000;
    g_tracking.on = 1;
    g_tracking.clock = t.wseen = 20;
    t.ww_epoch = g_tracking.epoch;
    return t;
}

int main(void) {
    fill();
    g_vk.alloc_cap = 256ull << 20;
    /* the video's picture: 1920 wide in 4 byte pixels, rows of whole pieces, 16 GOBs a block */
    Surf a = tex(1920, 136, 1, 1, 4, 4, 0, 1);
    upload_and_check(&a, GUEST, 1);
    /* rows that end inside a piece (27 pixels of 4 bytes, 108 bytes), and its mip levels with their shrunk blocks */
    Surf b = tex(27, 50, 1, 3, 4, 3, 0, 1);
    upload_and_check(&b, GUEST, 1);
    /* a 3D level with blocks of two slices, 2 byte texels: rows of 70 bytes */
    Surf c = tex(35, 20, 4, 1, 2, 1, 1, 1);
    upload_and_check(&c, GUEST, 1);
    /* a level gmem_ptr can't hand out in one piece: read through gmem_read, which gives zeros past the mapping */
    Surf d = tex(64, 64, 1, 1, 4, 2, 0, 1);
    upload_and_check(&d, 3 * 4096 + 100, 1);
    /* pitch linear */
    Surf e = tex(33, 9, 1, 1, 4, 0, 0, 0);
    upload_and_check(&e, GUEST, 1);
    /* no level-sized buffer of its own for a format the GPU takes as it is */
    stage_used = nregs = 0;
    g_big = 0;
    g_watch = 1920 * 136 * 4;
    tex_upload(&a);
    CHECK(g_big == 0);
    /* a converted format: deswizzled, then RGBA4 from 2 byte texels converted, both on the GPU */
    Surf f = tex(16, 8, 1, 1, 2, 0, 0, 1);
    f.f.conv = VKC_RGBA4;
    stage_used = copied_used = nregs = 0;
    convs = 0;
    copy_bpp = 4;
    tex_upload(&f);
    CHECK(nregs == 1 && convs == 1 && regs[0].bufferRowLength == 16);
    for (u32 y = 0; y < 8; y++)
        for (u32 x = 0; x < 16; x++) {
            u32 v = guest[bl_offset(x * 2, y, 0, 32, 8, 0, 0)] | guest[bl_offset(x * 2 + 1, y, 0, 32, 8, 0, 0)] << 8;
            u32 want = (v & 15) * 17 | ((v >> 4) & 15) * 17 << 8 | ((v >> 8) & 15) * 17 << 16 | (v >> 12) * 17 << 24;
            u32 got;
            memcpy(&got, copied + regs[0].bufferOffset + (y * 16 + x) * 4, 4);
            CHECK(got == want);
        }
    /* past the allocation cap, strips: under 64 KiB a row of the picture's blocks (16 GOBs high, 960 KiB) doesn't
       fit, so it goes a GOB row (8 rows, 7680 bytes each) at a time, each GOB of it gathered from its block, 17 of
       them; under 16 KiB the pitch-linear level in strips of 13 rows (its second level fits whole), and under 4 KiB
       the 3D level (blocks of 2 GOBs high and 2 slices deep, 4096 bytes a row of them) a row of blocks of 2 slices
       at a time */
    g_vk.alloc_cap = 64 << 10;
    upload_and_check(&a, GUEST, 17);
    g_vk.alloc_cap = 16 << 10;
    Surf g = tex(300, 40, 1, 2, 4, 0, 0, 0);
    upload_and_check(&g, GUEST, 4);
    g_vk.alloc_cap = 4 << 10;
    upload_and_check(&c, GUEST, 4);
    g_vk.alloc_cap = 16 << 10;
    /* a converted format is deswizzled and converted strip by strip: a row of blocks (32 rows) takes 4 KiB
       deswizzled and 8 KiB converted, so a strip is one, 10 of them */
    stage_used = copied_used = nregs = 0;
    convs = 0;
    copy_bpp = 4; /* the copies' texels are converted ones */
    Surf h = tex(64, 300, 1, 1, 2, 2, 0, 1);
    h.f.conv = VKC_RGBA4;
    CHECK(tex_upload(&h) && nregs == 10 && convs == 10);
    for (u32 i = 0; i < nregs; i++)
        for (u32 y = 0; y < regs[i].imageExtent.height; y++)
            for (u32 x = 0; x < 64; x++) {
                u32 gy = (u32)regs[i].imageOffset.y + y;
                u32 v = guest[bl_offset(x * 2, gy, 0, 128, 300, 2, 0)] | guest[bl_offset(x * 2 + 1, gy, 0, 128, 300, 2, 0)]
                                                                            << 8;
                u32 want = (v & 15) * 17 | ((v >> 4) & 15) * 17 << 8 | ((v >> 8) & 15) * 17 << 16 | (v >> 12) * 17 << 24;
                u32 got;
                memcpy(&got, copied + regs[i].bufferOffset + (y * 64 + x) * 4, 4);
                CHECK(got == want);
            }
    g_vk.alloc_cap = 256ull << 20;
    /* an ASTC level (4x4 blocks of 16 bytes) of 3 slices: its block rows as vk_astc_upload receives them,
       deswizzled on the GPU */
    Surf k = tex(60, 37, 3, 1, 16, 1, 1, 1);
    k.f.bw = k.f.bh = 4;
    k.f.conv = VKC_ASTC;
    stage_used = nregs = 0;
    CHECK(tex_upload(&k) && nregs == 0);
    for (u32 z = 0; z < 3; z++)
        for (u32 y = 0; y < 10; y++)
            for (u32 x = 0; x < 15 * 16; x++)
                CHECK(astc_out[((z * 10 + y) * 15) * 16 + x] == guest[bl_offset(x, y, z, 15 * 16, 10, 1, 1)]);
    /* a sparse texture's levels at least a tile in size have rows of whole tiles (tile_w). RGBA8 tiles of 128x128
       texels (8 GOBs across, blocks 16 high): 200 texels across (13 GOBs) take 16 at level 0, and levels 1 and 2 are
       narrower than a tile. Whole, in rows of blocks under 160 KiB (2 of them), and 8 GOB rows at a time under 64 KiB,
       each gathered from its blocks (4) */
    Surf sp = tex(200, 256, 1, 3, 4, 4, 0, 1);
    sp.tile_w = 3;
    upload_and_check(&sp, GUEST, 1);
    g_vk.alloc_cap = 160 << 10;
    upload_and_check(&sp, GUEST, 2);
    g_vk.alloc_cap = 64 << 10;
    upload_and_check(&sp, GUEST, 4);
    /* a volume's: RGBA8 tiles of 32x32x16 (2 GOBs across, blocks 4 high and 16 deep). 80x64x32 (5 GOBs) takes 6, and
       level 1, 40x32x16, a tile high and deep, 4. Whole, in a row of blocks of a slab under 512 KiB (2), and 4 GOB rows
       of a slice under 64 KiB (64) */
    Surf sv = tex(80, 64, 32, 2, 4, 2, 4, 1);
    sv.tile_w = 1;
    g_vk.alloc_cap = 256ull << 20;
    upload_and_check(&sv, GUEST, 1);
    g_vk.alloc_cap = 512 << 10;
    upload_and_check(&sv, GUEST, 2);
    g_vk.alloc_cap = 64 << 10;
    upload_and_check(&sv, GUEST, 64);
    g_vk.alloc_cap = 256ull << 20;
    /* its layers are whole tiles apart: 320x256 of 3 levels (24, 16 and 5 GOBs across), 544768 bytes of levels in 9
       tiles of 64 KiB, where the shrunk block alone would put the next layer at 548864 */
    CHECK(bl_layer_size(320, 256, 1, &sp.f, 4, 0, 3, 3) == 9 * 0x10000ull);
    /* and an ASTC level's block rows, deswizzled from rows of whole tiles: 4x4 blocks of 16 bytes in tiles 16 GOBs
       across and blocks 8 high, 75 blocks across (19 GOBs) taking 32, its 64 block rows a tile high */
    {
        Surf sa = tex(300, 256, 1, 1, 16, 3, 0, 1);
        sa.f.bw = sa.f.bh = 4;
        sa.f.conv = VKC_ASTC;
        sa.tile_w = 4;
        stage_used = nregs = 0;
        CHECK(tex_upload(&sa) && nregs == 0);
        for (u32 y = 0; y < 64; y++)
            for (u32 x = 0; x < 75 * 16; x++)
                CHECK(astc_out[(y * 75) * 16 + x] == guest[bl_offset_g(x, y, 0, 32, 64, 3, 0)]);
    }
    /* the change check, mem_hash: the same hash whether the GPU holds bytes somewhere (g_own_any: the texture read
       through vk_guest_peek) or not, and a byte past a page's first 64 changes it, as a row the CPU rewrites does */
    {
        const u64 at = GVA + 0x20000, size = 4 * 4096;
        u64 plain = mem_hash(at, size), owned;
        g_own_any = 1;
        owned = mem_hash(at, size);
        CHECK(owned == plain);
        guest[0x20000 + 4096 + 100] ^= 0x5A;
        CHECK(mem_hash(at, size) != owned);
        guest[0x20000 + 4096 + 100] ^= 0x5A;
        g_own_any = 0;
    }
    /* A clean scan does not advance the write clock. Its cached texture must
       still notice the very next stamp, including after several clean epochs. */
    {
        Surf t = tex(8, 2, 1, 1, 4, 0, 0, 0);
        t.size = t.pitch * t.h;
        copy_bpp = 4;
        stage_used = copied_used = nregs = 0;
        CHECK(tex_upload(&t));
        t.hash = mem_hash(t.gva, t.size);
        g_tracking.on = 1;
        g_tracking.clock = t.wseen = 10;
        for (u32 round = 0; round < 3; round++) {
            for (u32 clean = 0; clean < 2; clean++) {
                g_vk_present++;
                g_tracking.epoch++;
                stage_used = copied_used = nregs = 0;
                CHECK(tex_check(&t, t.gva) == &t && !nregs);
            }
            guest[0] ^= 0x5A;
            g_tracking.stamp = ++g_tracking.clock;
            g_vk_present++;
            g_tracking.epoch++;
            stage_used = copied_used = nregs = 0;
            CHECK(tex_check(&t, t.gva) == &t);
            CHECK(nregs == 1 && copied[regs[0].bufferOffset] == guest[0]);
        }
        g_tracking.on = 0;
    }
    /* a texture with holes in its mapping is no one host run: write tracking over each mapped run sees the CPU write
       a tile, where its settled checks would sample the texture (vk_sample_hash, which sees none of it here) */
    {
        Surf t = sparse_volume(1);
        const u32 x = 3 * 16 + 1, y = 9, z = 6; /* in block 3 of the second row of blocks: tile 2 */
        const u64 o = bl_offset(x * 4, y, z, 832, 64, 0, 4);
        CHECK(o / 0x10000 == 2);
        guest[o] ^= 0x5A;
        g_tracking.stamp = ++g_tracking.clock;
        g_tracking.at = o, g_tracking.len = 1;
        g_vk_present++;
        g_tracking.epoch++;
        stage_used = copied_used = nregs = 0;
        CHECK(tex_check(&t, t.gva) == &t);
        const u8* got = copied_texel(0, x, y, z);
        u32 want = guest_texel(0, x, y, z, 832, 64, 0, 4), have = 0;
        if (got) memcpy(&have, got, 4);
        CHECK(got && have == want);
        guest[o] ^= 0x5A;
        g_map.count = 0;
        memset(&g_tracking, 0, sizeof g_tracking);
    }
    printf("tex_upload_rows: %u checks passed\n", checks);
    return 0;
}
