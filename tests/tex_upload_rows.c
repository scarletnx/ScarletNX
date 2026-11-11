/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* tex_upload (vk/texture.c) from guest memory through the staging buffer and the GPU's deswizzles and conversions
   (a CPU model of them below, run as they are recorded, which checks the barriers around them) to the copies into
   the image, each copy's bytes checked against a walk of the block-linear layout byte by byte (bl_offset below): a
   level whose rows are whole 16-byte pieces, one whose rows end inside a piece, a 3D level with blocks of slices, mip
   levels, a level read through gmem_read, a pitch-linear level, a converted format, and a sparse texture's levels
   whose rows are whole tiles. No level takes a buffer of its size on the CPU. Levels past the allocation cap go in
   strips, each region checked where it lands, and an ASTC level's block rows reach vk_astc_upload strip by strip,
   deswizzled. Then the ASTC encoder between frames (astc_later_*): a BC7 texture held as ETC2 RGBA8 is queued only
   once sampled in VK_ASTC_LATER_READS presents and only where its alpha is one value a block, its blocks go from guest
   memory through the conversion's ASTC build into an image of its own, a frame's blocks at most a frame, which takes
   the texture's place once whole; a write drops it, and one of the ASTC image makes it ETC2 again */
#include <stdarg.h>
#include <stdio.h>
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
u32 g_nown;
void vk_guest_read(u64 gva, u64 size, u8* host, VkBuffer buf, u64 at) {
    (void)buf, (void)at;
    gmem_read(gva, host, size);
}
static u32 g_peeks;
void vk_guest_peek(u64 gva, void* dst, u64 size) {
    g_peeks++;
    gmem_read(gva, dst, size);
}
/* one GPU-owned range over every range asked, with this identity, while g_whole_tag isn't 0 */
static u64 g_whole_tag;
int vk_guest_owned_whole(u64 gva, u64 size, u64* tag) {
    (void)gva, (void)size;
    *tag = g_whole_tag;
    return g_whole_tag != 0;
}
/* no storage image publishes here: nothing is owned by one */
int vk_guest_owned_by(u64 gva, u64 size, u64 serial) {
    (void)gva, (void)size, (void)serial;
    return 0;
}
/* a texture with holes in its mapping (a virtual pool's, mapped a tile at a time): the runs g_map lists are what the
   GPU's page table maps, guest's bytes at the same offset (GP(cpu) is guest + cpu), and no texture is one host run */
u8* g_mem = guest;
static struct {
    u64 gva[8], n[8];
    u32 count;
} g_map;
static u32 g_walks; /* gmmu_pieces' calls */
void gmmu_pieces(u64 gva, u64 size, void (*fn)(void* ctx, u64 gva, u64 cpu, u64 n), void* ctx) {
    g_walks++;
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
u8* vk_stage_upload(u64 size, u64 align, VkBuffer* buf, u64* off) {
    return vk_stage(size, align, buf, off);
}
u8* vk_stage_guest_input(u64 gva, u64 source_size, u64 bytes, VkBuffer* buf, u64* off) {
    (void)gva; (void)source_size;
    return vk_stage_upload(bytes, vk_stage_align(), buf, off);
}
u64 vk_stage_align(void) { return 64; }
/* the scratch buffer the GPU deswizzles and converts into, which a CPU model of those dispatches writes here */
#define SCRATCH (8u << 20)
static u8 scratch[SCRATCH];
static u8* mem_of(VkBuffer b) { return b == STAGE_BUF ? stage : b == SCRATCH_BUF ? scratch : NULL; }
/* the copies into the image, with the bytes each copies taken when it is recorded (the scratch buffer is reused):
   a region's bufferOffset is where its bytes start in copied */
static VkBufferImageCopy regs[1024];
static VkImage regs_img[1024];               /* the image each region is copied into */
static u32 nregs, regs_bars[1024], all_bars; /* each region's count of vk_all_barrier before it */
static u8 copied[16u << 20];
static u64 copied_used;
static void VKAPI_CALL copy_to_image(VkCommandBuffer cb, VkBuffer b, VkImage img, VkImageLayout l, uint32_t n,
                                     const VkBufferImageCopy* r) {
    (void)cb, (void)l;
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
        regs_bars[nregs] = all_bars;
        regs_img[nregs] = img;
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
/* Uploading into the fixture's existing image never creates views or samplers. Keep the registry boundary
   explicit, and fail if an upload unexpectedly starts exercising object creation. */
VkResult vk_image_view_create(VkDevice device, const VkImageViewCreateInfo* info,
                               const VkAllocationCallbacks* allocation, VkImageView* view) {
    (void)device, (void)info, (void)allocation, (void)view;
    fputs("tex_upload_rows: unexpected image view creation\n", stderr);
    abort();
}
VkResult vk_sampler_create(VkDevice device, const VkSamplerCreateInfo* info,
                            const VkAllocationCallbacks* allocation, VkSampler* sampler) {
    (void)device, (void)info, (void)allocation, (void)sampler;
    fputs("tex_upload_rows: unexpected sampler creation\n", stderr);
    abort();
}
_Atomic int g_cs_any;
u32 g_vk_present;
static char g_log[1024]; /* the last line logged */
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(g_log, sizeof g_log, fmt, ap);
    va_end(ap);
}
int vk_ok_(VkResult r, const char* what, const char* file, int line) {
    (void)what, (void)file, (void)line;
    return r == VK_SUCCESS;
}
VkCommandBuffer vk_cmd(void) { return (VkCommandBuffer)(uintptr_t)0xCB; }
/* the BC7 encoder between frames (texture.c vk_tex_later_frame), which no case here runs: the textures are RGBA8 or
   held as such. Its calls link at every level, and abort if a case ever reaches them. The ASTC encoder of the same
   frames runs (check_astc_between_frames): its submissions are counted */
static u32 g_submits;
int vk_cmd_ready(void) { return 1; }
void vk_submit_recorded(void) { g_submits++; }
/* the BC7 cache: the key texture.c handed the last vk_astc_upload or vk_astc_upload_layers (keyed 0: none); takes,
   whether vk_astc_cached takes a level; owner_over, an owner on the pages of every range own_hash is asked about */
static struct {
    int keyed, takes, owner_over;
    u64 content[2];
} g_astc_key;
u64 vk_bc7_strip_texels(void) { abort(); }
/* the BC7 cache (astc.c, bc7_cache.c): it takes a level while g_astc_key.takes says so. The content is the bytes and
   layout words mixed in by FNV-1a, and the key that with the level's shape, which is all texture.c needs of them */
int vk_astc_cached(u32 w, u32 h, u32 depth) {
    (void)w, (void)h, (void)depth;
    return g_astc_key.takes;
}
/* a very large level, whose pieces the cache takes whatever its size (astc_quick_mtexels): none here */
int vk_astc_quick(u32 w, u32 h, u32 depth) {
    (void)w, (void)h, (void)depth;
    return 0;
}
void vk_bc7_content(u64 content[2], const void* p, u64 n) {
    for (u64 i = 0; i < n; i++) content[0] = (content[0] ^ ((const u8*)p)[i]) * 0x100000001b3ull;
    content[1] += n;
}
void vk_bc7_level_key(const u64 content[2], u32 w, u32 h, u32 depth, u32 layers, u32 bw, u32 bh, int srgb, u32 build,
                      u64 key[2]) {
    key[0] = content[0] ^ ((u64)w << 32 | h);
    key[1] = content[1] ^ ((u64)depth << 48 | (u64)layers << 32 | bw << 24 | bh << 16 | (u32)srgb << 8 | build);
}
u32 vk_bc7_build(u32 w, u32 h, u32 d) {
    (void)w, (void)h, (void)d;
    return 1;
}
int vk_bc7_cache_level(VkCommandBuffer cb, VkImage img, const u64 key[2], u32 level, u32 w, u32 h, u32 slices,
                       int is3d) {
    (void)cb, (void)img, (void)key, (void)level, (void)w, (void)h, (void)slices, (void)is3d;
    abort();
}
int vk_bc7_capture_level(VkCommandBuffer cb, VkImage img, const u64 key[2], u32 level, u32 w, u32 h, u32 slices,
                         int is3d) {
    (void)cb, (void)img, (void)key, (void)level, (void)w, (void)h, (void)slices, (void)is3d;
    abort();
}
/* the GPU's page table maps the guest's bytes at the same offset (GP(cpu) is guest + cpu), and an owner is on the pages
   of every range asked while g_astc_key.owner_over is set */
u64 gmmu_run(u64 gva, u64 max, u64* cpu) {
    if (gva < GVA || gva >= guest_end) return 0;
    *cpu = gva - GVA;
    return guest_end - gva < max ? guest_end - gva : max;
}
void gmem_need(u64 gva, u64 size) { (void)gva, (void)size; }
int own_hash(u64 addr, u64 size, u64* h) {
    (void)addr, (void)size;
    if (g_astc_key.owner_over) *h ^= 1;
    return g_astc_key.owner_over;
}
int vk_bc7_encode_image(VkCommandBuffer cb, VkImage src, VkImage dst, u32 level, u32 w, u32 h, u32 d, int is3d, u32 z,
                        u32 nz, u32 y, u32 rows) {
    (void)cb, (void)src, (void)dst, (void)level, (void)w, (void)h, (void)d, (void)is3d, (void)z, (void)nz, (void)y,
        (void)rows;
    abort();
}
/* surface.c's, for a storage image over a target, which no case here samples: an unoptimized build keeps the call, and
   the test links at every level with this, which aborts if a case ever reaches it */
int vk_rt_same_layout(const Surf* s, u32 w, u32 h, int bl, u32 bh, u32 pitch) {
    (void)s, (void)w, (void)h, (void)bl, (void)bh, (void)pitch;
    abort();
}
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
/* the ASTC upload asks for its block rows in strips of 3: they land in astc_out, slice after slice. While astc_append
   is set, each call's after the last one's, from astc_used on, its region and where its blocks start in astc_calls
   (the first ASTC_CALLS of them) */
static u8 astc_out[256 << 10];
#define ASTC_CALLS 16
static struct {
    u32 x, y, w, h, level, layer;
    u64 at;
} astc_calls[ASTC_CALLS];
static u32 nastc;
static u64 astc_used;
static int astc_append;
static void astc_key(const u64* content) {
    g_astc_key.keyed = content != NULL;
    if (content) memcpy(g_astc_key.content, content, sizeof g_astc_key.content);
}
int vk_astc_upload(VkCommandBuffer cb, Surf* t, u32 x, u32 y0, u32 w, u32 h, u32 depth, u32 level, u32 layer,
                   VkAstcRows rows, void* ctx, const u64* content) {
    u32 nx = (w + t->f.bw - 1) / t->f.bw, ny = (h + t->f.bh - 1) / t->f.bh;
    astc_key(content);
    if (nastc < ASTC_CALLS) {
        astc_calls[nastc].x = x, astc_calls[nastc].y = y0, astc_calls[nastc].w = w, astc_calls[nastc].h = h;
        astc_calls[nastc].level = level, astc_calls[nastc].layer = layer, astc_calls[nastc].at = astc_used;
        nastc++;
    }
    if (astc_append) {
        for (u32 z = 0; z < depth; z++)
            for (u32 y = 0; y < ny; y += 3) {
                u32 n = ny - y < 3 ? ny - y : 3;
                VkBuffer b;
                u64 off;
                CHECK(rows(ctx, &cb, z, 1, y, n, &b, &off) && mem_of(b));
                CHECK(astc_used + ((u64)z * ny + y + n) * nx * 16 <= sizeof astc_out);
                memcpy(astc_out + astc_used + ((u64)z * ny + y) * nx * 16, mem_of(b) + off, (u64)n * nx * 16);
            }
        astc_used += (u64)depth * ny * nx * 16;
        return 1;
    }
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
/* an ASTC array's level at once (vk_astc_upload_layers): its calls, and its block rows asked for 2 layers and 3 rows at
   a time, as strips take them, into astc_out layer after layer from astc_used, which goes on past them */
static u32 nlayered;
int vk_astc_upload_layers(VkCommandBuffer cb, Surf* t, u32 w, u32 h, u32 level, u32 layer, u32 layers, VkAstcRows rows,
                          void* ctx, const u64* content) {
    u32 nx = (w + t->f.bw - 1) / t->f.bw, ny = (h + t->f.bh - 1) / t->f.bh;
    (void)level;
    astc_key(content);
    nlayered++;
    for (u32 z = 0; z < layers; z += 2) {
        u32 nz = layers - z < 2 ? layers - z : 2;
        for (u32 y = 0; y < ny; y += 3) {
            u32 n = ny - y < 3 ? ny - y : 3;
            VkBuffer b;
            u64 off;
            CHECK(rows(ctx, &cb, z, nz, y, n, &b, &off) && mem_of(b));
            for (u32 k = 0; k < nz; k++) {
                u64 to = astc_used + (((u64)(layer + z + k) * ny + y) * nx * 16);
                CHECK(to + (u64)n * nx * 16 <= sizeof astc_out);
                memcpy(astc_out + to, mem_of(b) + off + (u64)k * n * nx * 16, (u64)n * nx * 16);
            }
        }
    }
    astc_used += (u64)layers * ny * nx * 16;
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
static u64 g_scratch_most; /* the most asked of the scratch buffer since it was set to 0 */
VkBuffer vk_bl_scratch(u64 size, u64* cap) {
    if (size > g_scratch_most) g_scratch_most = size;
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
/* the mark the model's conversion of a BC7 block leaves on each of its bytes: one for the blocks it is encoded again
   into, ETC2 RGBA8 or ASTC 4x4 */
static u8 bc7_mark(VkFormat f) { return f == VK_FORMAT_ASTC_4x4_UNORM_BLOCK ? 0xA5 : 0x5A; }
static u32 g_conv_blocks; /* BC7 blocks the model converted */
VkTcShape vk_tc_shape(const VkFmt* f, u32 w) {
    if (f->conv == VKC_BC7) /* a row of blocks into a row of blocks, 16 bytes each (texconv.c's tc_shape) */
        return (VkTcShape){
            .units = (w + 3) / 4, .out_texels = (w + 3) / 4 * 4, .out_bpp = 4, .out_rows = 1, .in_align = 16};
    CHECK(f->conv == VKC_RGBA4);
    return (VkTcShape){.units = (w + 1) / 2, .out_texels = (w + 1) / 2 * 2, .out_bpp = 4, .out_rows = 1, .in_align = 16};
}
u64 vk_tc_out_size(const VkTcShape* s, u32 rows, u32 slices) { return (u64)s->out_texels * 4 * rows * slices; }
int vk_tc_convert(VkCommandBuffer cb, const VkFmt* f, u32 w, u32 rows, u32 slices, VkBuffer in, u64 in_at,
                  u32 in_pitch, VkBuffer out, u64 out_at) {
    (void)cb;
    u32 ow = (w + 1) / 2 * 2;
    CHECK(mem_of(in) && mem_of(out) && (bar_state == 2 || (bar_state == 1 && !batch_moves)));
    if (f->conv == VKC_BC7) {
        const u32 cx = (w + 3) / 4;
        for (u32 y = 0; y < rows * slices; y++)
            for (u32 k = 0; k < cx * 16; k++)
                mem_of(out)[out_at + (u64)y * cx * 16 + k] =
                    mem_of(in)[in_at + (u64)y * in_pitch + k] ^ bc7_mark(f->vk);
        g_conv_blocks += cx * rows * slices;
        convs++;
        return 1;
    }
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
/* nothing here is written in place (uma.c): no texture is copied from an import (tex_copy_pending) */
int vk_uma_pending(u64 gva, u64 len) { return (void)gva, (void)len, 0; }
void vk_uma_gpu_reads(int on) { (void)on; }
int vk_uma_copy_pending(u64 cpu, u64 len, VkBuffer* buf, u64* off) {
    return (void)cpu, (void)len, (void)buf, (void)off, 0;
}
void vk_defer_image(VkImage img, VmaAllocation a, VkImageView v1, VkImageView v2) {
    (void)img, (void)a, (void)v1, (void)v2;
}
static u64 g_hashes;                   /* vk_hash's calls */
u64 vk_hash(const void* p, u64 size) { /* FNV-1a over the bytes: mem_hash's check below tells contents apart */
    const u8* b = (const u8*)p;
    g_hashes++;
    u64 h = 1469598103934665603ull;
    for (u64 i = 0; i < size; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
/* the drawn targets tex_take_drawn finds (check_take_skip): g_drawn_target over any range while set, its memory
   older than its pixels while g_drawn_fresh is; each search counted, and surface.c's count of their changes */
static Surf* g_drawn_target;
static int g_drawn_fresh;
static u32 g_drawn_searches;
static u64 g_take_gen_now = 1;
u64 vk_rt_take_gen(void) { return g_take_gen_now; }
int vk_rt_drawn_fresh(Surf* s) {
    (void)s;
    return g_drawn_fresh;
}
int vk_rt_drawn_in(u64 lo, u64 hi, Surf** out, int max) {
    (void)lo, (void)hi;
    g_drawn_searches++;
    if (!g_drawn_target) return 0;
    if (max > 0) out[0] = g_drawn_target;
    return 1;
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
VkImage vk_ms_unpack(VkCommandBuffer cb, Surf* s, u32 layer, u32 y0, u32 rows) {
    (void)cb, (void)s, (void)layer, (void)y0, (void)rows;
    return VK_NULL_HANDLE;
}
/* write tracking: the stamp is on guest's bytes [at, at + len), all of them when len is 0, or on the runs listed in
   run (nruns of them, in order, on pages apart) when there are any. As buffer.c's, it answers only for a host range */
static struct {
    int on, pending; /* pending: written, and taken in by the next check, which marks it with the clock's next value */
    u32 clock, stamp, epoch;
    u64 at, len;
    u32 nruns;
    struct {
        u64 at, len;
    } run[320];
} g_tracking;
static int stamped(u64 o, u64 n, u32 since) {
    if (g_tracking.stamp <= since) return 0;
    for (u32 i = 0; i < g_tracking.nruns; i++)
        if (o < g_tracking.run[i].at + g_tracking.run[i].len && g_tracking.run[i].at < o + n) return 1;
    return !g_tracking.nruns && (!g_tracking.len || (o < g_tracking.at + g_tracking.len && g_tracking.at < o + n));
}
int vk_written_since(const u8* host, u64 len, u32 since) {
    if (!g_tracking.on || !host || !len) return -1;
    if (g_tracking.pending) { /* buffer.c's ww_take_scanned: what scans found, marked as the check takes it in */
        g_tracking.stamp = ++g_tracking.clock;
        g_tracking.pending = 0;
    }
    return stamped((u64)(host - guest), len, since);
}
u32 vk_ww_now(void) { return g_tracking.clock; }
u32 vk_ww_epoch(void) { return g_tracking.epoch; }
/* the pages of the CPU's write that fall in [host, host + len), at their offsets in it */
void vk_written_runs(const u8* host, u64 len, u32 since, void (*fn)(void*, u64, u64), void* user) {
    u64 o = (u64)(host - guest), lo = g_tracking.at & ~4095ull, hi = (g_tracking.at + g_tracking.len + 4095) & ~4095ull;
    if (!stamped(o, len, since)) return;
    for (u32 i = 0; i < g_tracking.nruns; i++) {
        lo = g_tracking.run[i].at & ~4095ull, hi = (g_tracking.run[i].at + g_tracking.run[i].len + 4095) & ~4095ull;
        if (lo < o) lo = o;
        if (hi > o + len) hi = o + len;
        if (lo < hi) fn(user, lo - o, hi - lo);
    }
    if (g_tracking.nruns) return;
    if (!g_tracking.len || lo < o) lo = o;
    if (!g_tracking.len || hi > o + len) hi = o + len;
    fn(user, lo - o, hi - lo);
}
/* GPU-side writes (a mapping, an engine's): the last one, its generation on GPU bytes [at, at + len) */
static struct {
    u64 gen, at, len;
} g_gpu_writes;
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz) {
    return g_gpu_writes.gen > since && g_gpu_writes.at < a + sz && a < g_gpu_writes.at + g_gpu_writes.len;
}
void gpu_mem_dirty_runs(u64 since, u64 a, u64 sz, void (*fn)(void* ctx, u64 at, u64 n), void* ctx) {
    u64 lo = g_gpu_writes.at & ~4095ull, hi = (g_gpu_writes.at + g_gpu_writes.len + 4095) & ~4095ull;
    if (!gpu_mem_dirty_since(since, a, sz)) return;
    if (lo < a) lo = a;
    if (hi > a + sz) hi = a + sz;
    fn(ctx, lo, hi - lo);
}
u64 gpu_mem_generation(void) { return g_gpu_writes.gen; }
void vk_all_barrier(VkCommandBuffer cb) {
    (void)cb;
    all_bars++;
}
VkResult vmaCreateImage(VmaAllocator a, const VkImageCreateInfo* ii, const VmaAllocationCreateInfo* ai, VkImage* img,
                        VmaAllocation* mem, VmaAllocationInfo* info) {
    (void)a, (void)ii, (void)ai, (void)img, (void)mem, (void)info;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}
/* no image is made but while g_images says so: then one a call, its handle the count of them past 0x1000 */
static struct {
    int ok;
    u32 made;
    VkFormat format;
} g_images;
int vk_cache_image_create(const VkImageCreateInfo* ii, VkImage* img, VmaAllocation* mem, u64* bytes) {
    (void)mem;
    if (!g_images.ok) return 0;
    g_images.format = ii->format;
    *img = (VkImage)(uintptr_t)(0x1000 + ++g_images.made);
    *bytes = 1;
    return 1;
}
static VKAPI_ATTR void VKAPI_CALL no_format_properties(VkPhysicalDevice pd, VkFormat f, VkFormatProperties* p) {
    (void)pd, (void)f;
    memset(p, 0, sizeof *p);
}
/* format.c's, as it is there: a BC7 texture held as ETC2 RGBA8 is made ASTC 4x4 */
VkFormat vk_bcn_astc_later(const VkFmt* f) {
    return f->conv == VKC_BC7 && f->vk == VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK ? VK_FORMAT_ASTC_4x4_UNORM_BLOCK
                                                                              : VK_FORMAT_UNDEFINED;
}
/* images with memory only where it is bound: of the shape g_res.shape gives (none while its block is 0 texels), their
   pages bound as vk_image_resident is asked, each 64 KiB, those it gives memory marked 2 as memory.c marks them */
static struct {
    VkResidentShape shape;
    u8 bound[128];
    u32 pages;
} g_res;
int vk_resident_shape(const VkImageCreateInfo* ii, VkResidentShape* s) {
    (void)ii;
    *s = g_res.shape;
    return g_res.shape.block.width != 0;
}
int vk_image_create_resident(const VkImageCreateInfo* ii, const VkResidentShape* s, VkImage* img) {
    (void)ii, (void)s;
    memset(g_res.bound, 0, sizeof g_res.bound);
    *img = (VkImage)(uintptr_t)0x5E5;
    return g_res.shape.block.width != 0;
}
int vk_image_resident(VkImage img, u8* want, u32 n, u64* bytes) {
    (void)img;
    *bytes = 0;
    if (n > sizeof g_res.bound) return 0;
    g_res.pages = n;
    for (u32 i = 0; i < n; i++) {
        if (want[i] && !g_res.bound[i]) want[i] = 2;
        g_res.bound[i] = want[i] != 0;
        *bytes += g_res.bound[i] ? 0x10000 : 0;
    }
    return 1;
}
/* sparse residency: none here either */
int vk_image_create_sparse(const VkImageCreateInfo* ii, VkImage* img) {
    (void)ii, (void)img;
    return 0;
}
const VkSparseShape* vk_image_sparse_shape(VkImage img) {
    (void)img;
    return NULL;
}
int vk_image_sparse_bind(VkImage img, const u8* bits, u64* bytes) {
    (void)img, (void)bits, (void)bytes;
    return -1;
}
u64 vk_image_sparse_need(VkImage img, const u8* bits) {
    (void)img, (void)bits;
    return 0;
}
int vk_image_sparse_bound(VkImage img, u32 layer, u32 level, u32 y0, u32 y1, u32 z0, u32 z1, int all) {
    (void)img, (void)layer, (void)level, (void)y0, (void)y1, (void)z0, (void)z1, (void)all;
    return 1;
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

/* a piecewise texture's image, level 0 of 208x64x32 RGBA8 texels: what the regions recorded so far put in it */
static u8 model[208 * 64 * 32 * 4];
static void model_apply(void) {
    for (u32 i = 0; i < nregs; i++) {
        const VkBufferImageCopy* r = &regs[i];
        u32 rl = r->bufferRowLength ? r->bufferRowLength : r->imageExtent.width;
        u32 ih = r->bufferImageHeight ? r->bufferImageHeight : r->imageExtent.height;
        for (u32 z = 0; z < r->imageExtent.depth; z++)
            for (u32 y = 0; y < r->imageExtent.height; y++)
                for (u32 x = 0; x < r->imageExtent.width; x++)
                    memcpy(model + ((((u64)z + r->imageOffset.z) * 64 + y + r->imageOffset.y) * 208 + x +
                                    r->imageOffset.x) *
                                       4,
                           copied + r->bufferOffset + (((u64)z * ih + y) * rl + x) * 4, 4);
    }
}
/* texels of the model that aren't memory's where the volume is mapped (g_map) and zeros in its holes: how many */
static u32 model_wrong(void) {
    u32 wrong = 0;
    for (u32 z = 0; z < 32; z++)
        for (u32 y = 0; y < 64; y++)
            for (u32 x = 0; x < 208; x++) {
                u64 o = bl_offset(x * 4, y, z, 832, 64, 0, 4);
                u32 want = 0, have;
                for (u32 i = 0; i < g_map.count; i++)
                    if (GVA + o >= g_map.gva[i] && GVA + o < g_map.gva[i] + g_map.n[i])
                        want = guest_texel(0, x, y, z, 832, 64, 0, 4);
                memcpy(&have, model + (((u64)z * 64 + y) * 208 + x) * 4, 4);
                wrong += have != want;
            }
    return wrong;
}

static u32 g_image_copies;
static void count_copy_image(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, u32 n,
                             const VkImageCopy* r) {
    (void)cb, (void)src, (void)sl, (void)dst, (void)dl, (void)n, (void)r;
    g_image_copies++;
}
/* tex_take_drawn looks for the drawn targets over a texture again only once something it could take may have changed
   (vk_rt_take_gen) or the texture was loaded again (its wgen): with nothing over it, with a target whose memory is
   newer than its pixels, and with one it took, a second bind searches nothing. A search that copied a target leaves it
   settled too: the target's next draw moves the count */
static void check_take_skip(void) {
    Surf t = tex(64, 64, 1, 1, 4, 2, 0, 1), s = tex(64, 64, 1, 1, 4, 2, 0, 1);
    s.is_rt = 1;
    s.sw = s.w;
    s.sh = s.h;
    s.img = (VkImage)(uintptr_t)0x2B7;
    t.wgen = 7;
    s.wgen = 3;
    vkCmdCopyImage = count_copy_image;
    g_drawn_searches = g_image_copies = 0;
    tex_take_drawn(&t);
    tex_take_drawn(&t);
    CHECK(g_drawn_searches == 1); /* nothing over it, nothing moved */
    g_take_gen_now++;
    tex_take_drawn(&t);
    CHECK(g_drawn_searches == 2); /* the count moved */
    t.wgen++;
    tex_take_drawn(&t);
    CHECK(g_drawn_searches == 3); /* loaded again */
    g_drawn_target = &s;
    g_take_gen_now++;
    tex_take_drawn(&t);
    tex_take_drawn(&t);
    CHECK(g_drawn_searches == 4 && g_image_copies == 0); /* memory newer than the target's pixels: nothing to take */
    g_drawn_fresh = 1;
    s.wgen++;
    g_take_gen_now++; /* drawn again */
    tex_take_drawn(&t);
    tex_take_drawn(&t);
    CHECK(g_drawn_searches == 5 && g_image_copies == 1); /* taken once */
    g_drawn_target = NULL;
    g_drawn_fresh = 0;
    vkCmdCopyImage = NULL;
}

/* the regions recorded since nregs was cleared, each against memory: the texels or blocks of level l of layer
   `layer` of t it holds, where it puts them, through the layout walked byte by byte. 1 when (layer, l, u, v), a texel
   or block of a level, is in one of them */
static int region_has(u32 layer, u32 l, u32 u, u32 v, u32 bw, u32 bh) {
    for (u32 i = 0; i < nregs; i++) {
        const VkBufferImageCopy* r = &regs[i];
        u32 x0 = (u32)r->imageOffset.x / bw, y0 = (u32)r->imageOffset.y / bh;
        if (r->imageSubresource.mipLevel == l && r->imageSubresource.baseArrayLayer == layer &&
            u - x0 < (r->imageExtent.width + bw - 1) / bw && v - y0 < (r->imageExtent.height + bh - 1) / bh)
            return 1;
    }
    return 0;
}
static void regions_match(const Surf* t) {
    const VkFmt* f = &t->f;
    u32 d3 = t->type == VK_IMAGE_VIEW_TYPE_3D ? t->depth3d : 1;
    for (u32 i = 0; i < nregs; i++) {
        const VkBufferImageCopy* r = &regs[i];
        u32 l = r->imageSubresource.mipLevel, layer = r->imageSubresource.baseArrayLayer, lbh = 0, lbd = 0;
        u64 at = (u64)layer * t->layer_size;
        for (u32 k = 0; k < l; k++) at += bl_level_size(t->w, t->h, d3, f, t->bh, t->bd, t->tile_w, k, NULL, NULL);
        u32 lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1, ld = d3 >> l ? d3 >> l : 1;
        u32 tw = (lw + f->bw - 1) / f->bw, th = (lh + f->bh - 1) / f->bh, rowb = tw * f->bpp;
        if (t->bl) bl_level_size(t->w, t->h, d3, f, t->bh, t->bd, t->tile_w, l, &lbh, &lbd);
        u32 gw = level_gobs(t, rowb, th, ld), u0 = (u32)r->imageOffset.x / f->bw, v0 = (u32)r->imageOffset.y / f->bh;
        u32 z0 = (u32)r->imageOffset.z, uw = (r->imageExtent.width + f->bw - 1) / f->bw;
        u32 uh = (r->imageExtent.height + f->bh - 1) / f->bh;
        u64 rl = (u64)(r->bufferRowLength ? r->bufferRowLength : r->imageExtent.width) / f->bw * f->bpp;
        u64 ih = (r->bufferImageHeight ? r->bufferImageHeight : r->imageExtent.height) / f->bh;
        CHECK(layer < t->layers && u0 + uw <= tw && v0 + uh <= th && z0 + r->imageExtent.depth <= ld &&
              rl >= uw * f->bpp);
        for (u32 z = 0; z < r->imageExtent.depth; z++)
            for (u32 y = 0; y < uh; y++)
                for (u32 x = 0; x < uw * f->bpp; x++) {
                    u64 o = t->bl ? bl_offset_g(u0 * f->bpp + x, v0 + y, z0 + z, gw, th, lbh, lbd)
                                  : ((u64)(z0 + z) * th + v0 + y) * t->pitch + u0 * f->bpp + x;
                    CHECK(copied[r->bufferOffset + ((u64)z * ih + y) * rl + x] == guest[at + o]);
                }
    }
}
/* the check that follows a write: a new frame and epoch, the regions recorded from none, and the loads it made
   (vk_tex_stats, vk_tex_partial_stats) as bytes loaded, and how many of them some blocks alone */
static void check_after(Surf* t, u64* bytes, u64* partial) {
    u64 loads, b0, b1, p0, p1, pb;
    g_vk_present++;
    g_tracking.epoch++;
    stage_used = copied_used = nregs = 0;
    vk_tex_stats(&loads, &b0);
    vk_tex_partial_stats(&p0, &pb);
    CHECK(tex_check(t, t->gva) == t);
    vk_tex_stats(&loads, &b1);
    vk_tex_partial_stats(&p1, &pb);
    *bytes = b1 - b0;
    *partial = p1 - p0;
    regions_match(t);
}
/* the CPU writes n bytes of guest at o (each xored with 0x5A), which the next check takes in */
static void cpu_write(u64 o, u32 n) {
    for (u32 i = 0; i < n; i++) guest[o + i] ^= 0x5A;
    g_tracking.pending = 1;
    g_tracking.at = o, g_tracking.len = n;
    g_tracking.nruns = 0;
}
/* a check that finds nothing loads nothing */
static void check_quiet(Surf* t) {
    u64 bytes, partial;
    check_after(t, &bytes, &partial);
    CHECK(bytes == 0 && partial == 0 && nregs == 0);
}
/* a texture loaded whole: write tracking answers for it from its clock as it is now (40 when it starts) */
static void loaded(Surf* t) {
    copy_bpp = t->f.bpp, copy_bw = t->f.bw, copy_bh = t->f.bh;
    stage_used = copied_used = nregs = 0;
    CHECK(tex_upload(t) && t->upload_valid);
    t->hash = mem_hash(t->gva, t->size);
    t->checked = g_vk_present;
    t->gw = gpu_mem_generation();
    if (!g_tracking.on) g_tracking.clock = 40;
    g_tracking.on = 1;
    t->wseen = g_tracking.clock;
    t->ww_epoch = g_tracking.epoch;
}
/* A block linear texture loaded whole takes the CPU's and the GPU's writes a block at a time (tex_check_pieces,
   tex_patch): RGBA8 256x256 of 9 levels in 2 layers, its big levels in blocks 16 GOBs high (8 KiB, two pages). Each
   write loads the blocks of its pages, which hold memory's bytes, and nothing else: 16 bytes in a block's second page
   load the block (both pages); a write across levels 3 and 4 the blocks of both pages, levels 4 to 8 in the second (the
   mip tail); the GPU's write and the CPU's in one check both. More pages than a check keeps load less than the whole
   texture of 512x1536. Every page written with the bytes it had is more than half the texture: hashed while the hash
   is the image's, which loads nothing, else loaded whole. Without write tracking a write loads all of it, found by
   its hash */
static void check_block_linear_changes(void) {
    Surf t = tex(256, 256, 1, 9, 4, 4, 0, 1);
    u64 lvl[10] = {0}, bytes, partial;
    t.layers = 2;
    t.type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    t.layer_size = bl_layer_size(256, 256, 1, &t.f, 4, 0, 0, 9);
    for (u32 l = 0; l < 9; l++) lvl[l + 1] = lvl[l] + bl_level_size(256, 256, 1, &t.f, 4, 0, 0, l, NULL, NULL);
    t.size = t.layer_size + lvl[9];
    CHECK(lvl[3] == 0x54000 && lvl[4] == 0x55000 && lvl[9] - lvl[4] == 3 * 1024 && t.layer_size == 0x56000);
    loaded(&t);
    /* every page written with the bytes it had: more than half the texture, which its hash finds unchanged */
    g_tracking.pending = 1;
    g_tracking.at = 0, g_tracking.len = t.size;
    g_hashes = 0;
    check_after(&t, &bytes, &partial);
    CHECK(bytes == 0 && partial == 0 && nregs == 0 && g_hashes);
    /* 16 bytes of texel (100, 200) of level 0 of layer 1: GOB column 6, the second row of blocks, its second page */
    const u64 a = t.layer_size + bl_offset(400, 200, 0, 1024, 256, 4, 0);
    CHECK(a % 8192 >= 4096);
    cpu_write(a, 16);
    check_after(&t, &bytes, &partial);
    CHECK(bytes == 8192 && partial == 1 && nregs == 1 && region_has(1, 0, 100, 200, 1, 1));
    CHECK(regs[0].imageOffset.x == 96 && regs[0].imageOffset.y == 128 && regs[0].imageExtent.width == 16 &&
          regs[0].imageExtent.height == 128);
    check_quiet(&t);
    /* 32 bytes across levels 3 and 4 of layer 0: all of level 3 (its two blocks, a run of a row of blocks) and levels 4
       to 8, each a region */
    cpu_write(lvl[4] - 16, 32);
    check_after(&t, &bytes, &partial);
    CHECK(bytes == 4096 + 3 * 1024 && partial == 1 && nregs == 6);
    for (u32 l = 3; l < 9; l++) CHECK(region_has(0, l, 0, 0, 1, 1));
    check_quiet(&t);
    /* the GPU writes 64 bytes of level 1 of layer 1 and the CPU 4 of level 0 of layer 0 before one check */
    g_gpu_writes.gen = gpu_mem_generation() + 1;
    g_gpu_writes.at = GVA + t.layer_size + lvl[1] + bl_offset(64, 100, 0, 512, 128, 4, 0), g_gpu_writes.len = 64;
    for (u32 i = 0; i < 64; i++) guest[g_gpu_writes.at - GVA + i] ^= 0x33;
    cpu_write(bl_offset(1000, 3, 0, 1024, 256, 4, 0), 4);
    check_after(&t, &bytes, &partial);
    CHECK(bytes == 2 * 8192 && partial == 1 && nregs == 2 && region_has(1, 1, 16, 100, 1, 1) &&
          region_has(0, 0, 250, 3, 1, 1));
    check_quiet(&t);
    /* 260 pages apart, each a byte written: more than the 256 pieces a check keeps. The closest go as one, over the
       pages between them (the first 5 written, a run of 5 blocks), the others a block each: less than the whole */
    Surf big = tex(512, 1536, 1, 1, 4, 4, 0, 1);
    big.size = bl_level_size(512, 1536, 1, &big.f, 4, 0, 0, 0, NULL, NULL);
    CHECK(big.size == 3u << 20);
    loaded(&big);
    g_tracking.pending = 1;
    g_tracking.nruns = 260;
    for (u32 k = 0; k < 260; k++) {
        guest[k * 0x2000 + 0x123] ^= 0x5A;
        g_tracking.run[k].at = k * 0x2000 + 0x123, g_tracking.run[k].len = 1;
    }
    check_after(&big, &bytes, &partial);
    CHECK(partial == 1 && bytes == 260 * 8192 && nregs == 256);
    for (u32 k = 0; k < 260; k++) {
        u32 b = k * 0x2000 / 8192; /* the block, a GOB across and 128 rows */
        CHECK(region_has(0, 0, b % 32 * 16, b / 32 * 128, 1, 1));
    }
    check_quiet(&big);
    /* every page written with the bytes it had again, with the hash no longer the image's: loaded whole, unhashed */
    g_tracking.pending = 1;
    g_tracking.at = 0, g_tracking.len = t.size;
    g_hashes = 0;
    check_after(&t, &bytes, &partial);
    CHECK(bytes == t.size && partial == 0 && g_hashes == 0);
    /* without write tracking the same texture's write is found by its hash, which loads all of it */
    memset(&g_tracking, 0, sizeof g_tracking);
    t.stable = 0;
    guest[a] ^= 0x5A;
    check_after(&t, &bytes, &partial);
    CHECK(bytes == t.size && partial == 0);
    memset(&g_gpu_writes, 0, sizeof g_gpu_writes);
}
/* A texture loaded whole in two runs of host memory and no holes, for which write tracking can't tell: one walk of its
   runs finds that (tex_check_pieces), and its contents are checked, without a second walk to ask again */
static void check_untracked_runs(void) {
    Surf t = tex(64, 64, 1, 1, 4, 2, 0, 1);
    u64 bytes, partial;
    t.size = bl_level_size(64, 64, 1, &t.f, 2, 0, 0, 0, NULL, NULL);
    loaded(&t);
    g_map.count = 2;
    g_map.gva[0] = GVA, g_map.n[0] = 8192;
    g_map.gva[1] = GVA + 8192, g_map.n[1] = t.size - 8192;
    g_walks = 0;
    check_after(&t, &bytes, &partial);
    CHECK(g_walks == 1 && bytes == 0);
    g_map.count = 0;
    memset(&g_tracking, 0, sizeof g_tracking);
}

/* A pitch linear texture loaded whole takes a write as the rows it lies in (tex_patch_pitch): of a row, the texels its
   pages cover, and rows after one another that cover the same texels as one region. RGBA8 3000x300 in rows of 12288
   bytes (three pages): a 64x64 region at (1100, 50), whose bytes are in the second page of each row, is one region of
   texels 1024 to 2047 of its 64 rows. A byte in each of its 300 rows, more pieces than a check keeps: the closest
   join, rows 0 to 44 whole, and the others' first page each as one region. 1000x100 in rows of 4352 bytes, which pages
   cut: a byte of the second page loads rows 0 and 1 whole. 2000x200 in rows of 8448 bytes: a byte at the start of
   each of rows 10 to 29, each in a page of its own that it shares with the row before, loads rows 9 to 29 once, in
   three regions (row 16 starts a page). Without write tracking a write loads all of it */
static void check_pitch_linear_changes(void) {
    Surf t = tex(3000, 300, 1, 1, 4, 0, 0, 0), c = tex(1000, 100, 1, 1, 4, 0, 0, 0);
    u64 bytes, partial;
    t.pitch = 12288;
    t.layer_size = t.size = 12288 * 300;
    loaded(&t);
    g_tracking.pending = 1;
    g_tracking.nruns = 64;
    for (u32 k = 0; k < 64; k++) {
        u64 o = (u64)(50 + k) * 12288 + 1100 * 4;
        for (u32 i = 0; i < 256; i++) guest[o + i] ^= 0x5A;
        g_tracking.run[k].at = o, g_tracking.run[k].len = 256;
    }
    check_after(&t, &bytes, &partial);
    CHECK(partial == 1 && nregs == 1 && bytes == 64 * 4096);
    CHECK(regs[0].imageOffset.x == 1024 && regs[0].imageOffset.y == 50 && regs[0].imageExtent.width == 1024 &&
          regs[0].imageExtent.height == 64);
    check_quiet(&t);
    g_tracking.pending = 1;
    g_tracking.nruns = 300;
    for (u32 k = 0; k < 300; k++) {
        guest[k * 12288 + 100] ^= 0x5A;
        g_tracking.run[k].at = k * 12288 + 100, g_tracking.run[k].len = 1;
    }
    check_after(&t, &bytes, &partial);
    CHECK(partial == 1 && nregs == 2 && bytes == 45 * 12000 + 255 * 4096);
    CHECK(regs[0].imageExtent.width == 3000 && regs[0].imageExtent.height == 45 && regs[1].imageOffset.y == 45 &&
          regs[1].imageExtent.width == 1024 && regs[1].imageExtent.height == 255);
    check_quiet(&t);
    c.pitch = 4352;
    c.layer_size = c.size = 4352 * 100;
    loaded(&c);
    cpu_write(4096 + 100, 1);
    check_after(&c, &bytes, &partial);
    CHECK(partial == 1 && nregs == 1 && bytes == 2 * 4000 && regs[0].imageOffset.y == 0 &&
          regs[0].imageExtent.height == 2 && regs[0].imageExtent.width == 1000);
    check_quiet(&c);
    Surf d = tex(2000, 200, 1, 1, 4, 0, 0, 0);
    d.pitch = 8448;
    d.layer_size = d.size = 8448 * 200;
    loaded(&d);
    g_tracking.pending = 1;
    g_tracking.nruns = 20;
    for (u32 k = 0; k < 20; k++) {
        guest[(10 + k) * 8448] ^= 0x5A;
        g_tracking.run[k].at = (10 + k) * 8448, g_tracking.run[k].len = 1;
    }
    check_after(&d, &bytes, &partial);
    CHECK(partial == 1 && nregs == 3 && bytes == 7 * 8000 + 4096 + 14 * 8000);
    CHECK(regs[0].imageOffset.y == 9 && regs[0].imageExtent.height == 7 && regs[2].imageOffset.y == 16 &&
          regs[2].imageExtent.height == 14);
    check_quiet(&d);
    memset(&g_tracking, 0, sizeof g_tracking);
    t.stable = 0;
    guest[100] ^= 0x5A;
    check_after(&t, &bytes, &partial);
    CHECK(bytes == t.size && partial == 0);
}
/* An ASTC texture loaded whole takes a write as the blocks of the runs of GOBs its pages are in, decoded again
   (level_patch_astc): 6x6 blocks, 600x384 texels, 100x64 blocks in rows of 25 GOBs, blocks 8 GOBs high (4 KiB, a
   page). A byte of GOB column 7 decodes its run, 4 ASTC blocks of 64 rows, 24x384 texels at (168, 0), on whole 4x4
   blocks for BC7 (168 and 24 are multiples of 12); one of the last column, the level's edge, 24 texels at 576 */
static void check_astc_changes(void) {
    Surf t = tex(600, 384, 1, 1, 16, 3, 0, 1);
    u64 bytes, partial;
    t.f.bw = t.f.bh = 6;
    t.f.conv = VKC_ASTC;
    t.f.vk = VK_FORMAT_BC7_UNORM_BLOCK;
    t.size = t.layer_size = bl_level_size(600, 384, 1, &t.f, 3, 0, 0, 0, NULL, NULL);
    CHECK(t.size == 25 * 4096);
    loaded(&t);
    for (u32 column = 7; column < 32; column += 17) { /* 7, then 24 */
        cpu_write(column * 4096 + 100, 1);
        nastc = 0;
        astc_used = 0;
        astc_append = 1;
        check_after(&t, &bytes, &partial);
        CHECK(partial == 1 && bytes == 4096 && nastc == 1 && astc_calls[0].x == column * 24 && astc_calls[0].y == 0 &&
              astc_calls[0].w == 24 && astc_calls[0].h == 384 && astc_calls[0].x % 12 == 0);
        for (u32 y = 0; y < 64; y++)
            for (u32 x = 0; x < 4 * 16; x++)
                CHECK(astc_out[y * 64 + x] == guest[bl_offset_g(column * 64 + x, y, 0, 25, 64, 3, 0)]);
        check_quiet(&t);
    }
    astc_append = 0;
    memset(&g_tracking, 0, sizeof g_tracking);
}

/* the regions recorded into a texture of 4-byte texels with layers (rows of whole tiles where tile_w says) against
   guest's layout: every texel each copies where the layout has it in its level and layer. The texels each copied of
   each level of each layer go in covered */
static void check_layer_regions(const Surf* t, u32 covered[2][16]) {
    memset(covered, 0, 2 * 16 * sizeof(u32));
    for (u32 i = 0; i < nregs; i++) {
        const VkBufferImageCopy* r = &regs[i];
        const u32 l = r->imageSubresource.mipLevel, k = r->imageSubresource.baseArrayLayer;
        const u32 lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1;
        const u32 rl = r->bufferRowLength ? r->bufferRowLength : r->imageExtent.width;
        u32 lbh, lbd;
        u64 at = k * t->layer_size;
        CHECK(k < 2 && l < t->levels && r->imageExtent.depth == 1);
        for (u32 j = 0; j < l; j++) at += bl_level_size(t->w, t->h, 1, &t->f, t->bh, t->bd, t->tile_w, j, NULL, NULL);
        bl_level_blocks(t->h, 1, &t->f, t->bh, t->bd, l, &lbh, &lbd);
        const u32 gobs = level_gobs(t, lw * 4, lh, 1);
        for (u32 y = 0; y < r->imageExtent.height; y++)
            for (u32 x = 0; x < r->imageExtent.width; x++) {
                const u32 tx = (u32)r->imageOffset.x + x, ty = (u32)r->imageOffset.y + y;
                CHECK(tx < lw && ty < lh);
                CHECK(!memcmp(copied + r->bufferOffset + ((u64)y * rl + x) * 4,
                              guest + at + bl_offset_g(tx * 4, ty, 0, gobs, lh, lbh, 0), 4));
            }
        covered[k][l] += r->imageExtent.width * r->imageExtent.height;
    }
}
/* A texture with memory only where it is mapped, of 2 layers: 256x256 RGBA8 in tiles of 128x128 texels (8 GOBs
   across, blocks 16 high), 9 levels, the device's blocks a tile, its mip tail from level 2 and one a layer. Each layer
   is 4 bricks of level 0, 1 of level 1, then the tail's levels in a tile of their own: 6 tiles. Mapped: tile 1 of
   level 0 of layer 0, level 1 of layer 1 and layer 1's mip tail, which take memory and are loaded, each texel in its
   level and layer and nothing else. Then 4 KiB of layer 0's mip tail mapped: the tail gets memory and is loaded whole,
   its texels undefined until then */
static void check_resident_layers(void) {
    Surf t = tex(256, 256, 1, 9, 4, 4, 0, 1);
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    u32 covered[2][16];
    t.tile_w = 3;
    t.layers = 2;
    t.layer_size = bl_layer_size(256, 256, 1, &t.f, 4, 0, 3, 9);
    CHECK(t.layer_size == 6 * 0x10000ull);
    for (u32 l = 0; l < 9; l++) t.size += bl_level_size(256, 256, 1, &t.f, 4, 0, 3, l, NULL, NULL);
    t.size += t.layer_size;
    g_res.shape = (VkResidentShape){{128, 128, 1}, 2, 2};
    CHECK(resident_image(&t, &ii) && t.bricks == 5 && t.tail_first == 2 && t.tails == 2 && t.brick_bytes == 0x10000);
    g_map.count = 3;
    g_map.gva[0] = GVA + 0x10000, g_map.n[0] = 0x10000;
    g_map.gva[1] = GVA + 0xA0000, g_map.n[1] = 0x10000;
    g_map.gva[2] = GVA + 0xB0000, g_map.n[2] = 0x10000;
    copy_bpp = 4, copy_bw = copy_bh = 1;
    stage_used = copied_used = nregs = 0;
    CHECK(tex_upload(&t) && g_res.pages == 12);
    static const u8 bound[12] = {0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1};
    CHECK(!memcmp(g_res.bound, bound, 12));
    check_layer_regions(&t, covered);
    CHECK(covered[0][0] == 128 * 128 && covered[1][1] == 128 * 128 && !covered[1][0] && !covered[0][1]);
    for (u32 l = 2; l < 9; l++) CHECK(!covered[0][l] && covered[1][l] == (256u >> l) * (256u >> l));
    /* layer 0's tail mapped by 4 KiB of it: a piece of 4 KiB, its tail bound and loaded whole */
    TexPieces p = {.count = 1};
    p.piece[0].at = 0x50000, p.piece[0].n = 0x1000;
    g_map.count = 4;
    g_map.gva[3] = GVA + 0x50000, g_map.n[3] = 0x1000;
    t.upload_valid = 1;
    stage_used = copied_used = nregs = 0;
    CHECK(tex_patch(&t, &p) && g_res.bound[10] && g_res.bound[11]);
    check_layer_regions(&t, covered);
    for (u32 l = 0; l < 9; l++) CHECK(!covered[1][l] && covered[0][l] == (l < 2 ? 0 : (256u >> l) * (256u >> l)));
    g_map.count = 0;
    g_res.shape = (VkResidentShape){{0, 0, 0}, 0, 0};
}
/* call c of the ASTC upload (astc_calls) is the region w x h at (x, y) of level l of t, layer 0, and its blocks in
   astc_out those of that region in guest memory, laid out as the level is */
static void check_astc_call(const Surf* t, u32 c, u32 x, u32 y, u32 w, u32 h, u32 l) {
    const u32 lw = t->w >> l ? t->w >> l : 1, lh = t->h >> l ? t->h >> l : 1, th = (lh + 3) / 4, nx = (w + 3) / 4;
    u32 lbh, lbd;
    u64 at = 0;
    CHECK(c < nastc && astc_calls[c].x == x && astc_calls[c].y == y && astc_calls[c].w == w && astc_calls[c].h == h &&
          astc_calls[c].level == l && astc_calls[c].layer == 0);
    for (u32 j = 0; j < l; j++) at += bl_level_size(t->w, t->h, 1, &t->f, t->bh, t->bd, t->tile_w, j, NULL, NULL);
    bl_level_blocks(t->h, 1, &t->f, t->bh, t->bd, l, &lbh, &lbd);
    const u32 gobs = level_gobs(t, (lw + 3) / 4 * 16, th, 1);
    for (u32 by = 0; by < (h + 3) / 4; by++)
        for (u32 b = 0; b < nx * 16; b++)
            CHECK(astc_out[astc_calls[c].at + (u64)by * nx * 16 + b] ==
                  guest[at + bl_offset_g(x / 4 * 16 + b, y / 4 + by, 0, gobs, th, lbh, 0)]);
}
/* An ASTC 4x4 texture held as BC7 with memory only where it is mapped: 1024x1024, 11 levels, in tiles of 64x64
   blocks (16 GOBs across, blocks 8 high), the device's blocks BC7's 256x256 texels, a tile each, its mip tail from
   level 3: 16 bricks of level 0, 4 of level 1, 1 of level 2, then the tail. Tile (1, 0) of level 0, (1, 1) of level 1
   and the tail mapped: its whole upload decodes (and encodes) each bound tile's blocks into its 256x256 texels, and
   the tail's levels, and copies none of their bytes into the image as they are. Then 16 bytes of tile (1, 0) written:
   the run of blocks they lie in decoded again, and nothing for 16 of a hole; a tile of level 0 mapped: bound, and
   decoded whole. An ASTC 8x8 one, the device's blocks 4 GOBs high in its layout where its blocks are 8, binds them in
   stacks of two (resident_stacks): 85 bricks of 16 KiB, and its first tile mapped, 64 KiB, binds the 2x2 device
   blocks it covers, 0, 1, 8 and 9. A 6x6 one, whose blocks aren't whole in 256 texels, and one the device makes no
   sparse image of are made whole, the log line saying the format and why */
static void check_resident_astc(void) {
    Surf t = tex(1024, 1024, 1, 11, 16, 3, 0, 1);
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    t.f.bw = t.f.bh = 4;
    t.f.conv = VKC_ASTC;
    t.f.vk = VK_FORMAT_BC7_UNORM_BLOCK;
    t.tile_w = 4;
    t.layer_size = bl_layer_size(1024, 1024, 1, &t.f, 3, 0, 4, 11);
    for (u32 l = 0; l < 11; l++) t.size += bl_level_size(1024, 1024, 1, &t.f, 3, 0, 4, l, NULL, NULL);
    CHECK(t.layer_size == 22 * 0x10000ull);
    g_res.shape = (VkResidentShape){{256, 256, 1}, 3, 1};
    CHECK(resident_image(&t, &ii) && t.bricks == 21 && t.tail_first == 3 && t.tails == 1 && t.brick_bytes == 0x10000);
    g_map.count = 3;
    g_map.gva[0] = GVA + 0x10000, g_map.n[0] = 0x10000;  /* tile (1, 0) of level 0 */
    g_map.gva[1] = GVA + 0x130000, g_map.n[1] = 0x10000; /* (1, 1) of level 1, after level 0's 16 tiles */
    g_map.gva[2] = GVA + 0x150000, g_map.n[2] = 0x10000; /* the tail */
    copy_bpp = 16, copy_bw = copy_bh = 4;
    stage_used = copied_used = nregs = 0;
    nastc = 0, astc_used = 0, astc_append = 1;
    CHECK(tex_upload(&t) && g_res.pages == 22);
    for (u32 i = 0; i < 22; i++) CHECK(g_res.bound[i] == (i == 1 || i == 19 || i == 21));
    CHECK(nregs == 0 && nastc == 2 + 8); /* the two tiles, then the tail's 8 levels */
    check_astc_call(&t, 0, 256, 0, 256, 256, 0);
    check_astc_call(&t, 1, 256, 256, 256, 256, 1);
    for (u32 l = 3; l < 11; l++) check_astc_call(&t, 2 + l - 3, 0, 0, 1024 >> l, 1024 >> l, l);
    /* 16 bytes of tile (1, 0), in its third column of GOBs, and 16 of tile (2, 0), a hole */
    TexPieces p = {.count = 2};
    p.piece[0].at = 0x12000, p.piece[0].n = 16;
    p.piece[1].at = 0x20100, p.piece[1].n = 16;
    t.upload_valid = 1;
    stage_used = copied_used = nregs = 0;
    nastc = 0, astc_used = 0;
    CHECK(tex_patch(&t, &p) && nregs == 0 && nastc == 1);
    check_astc_call(&t, 0, 288, 0, 16, 256, 0);
    /* tile (2, 0) mapped, the GPU's records naming its 64 KiB */
    g_map.count = 4;
    g_map.gva[3] = GVA + 0x20000, g_map.n[3] = 0x10000;
    p.count = 1, p.piece[0].at = 0x20000, p.piece[0].n = 0x10000;
    stage_used = copied_used = nregs = 0;
    nastc = 0, astc_used = 0;
    CHECK(tex_patch(&t, &p) && g_res.bound[2] && nregs == 0 && nastc == 1);
    check_astc_call(&t, 0, 512, 0, 256, 256, 0);
    g_map.count = 0;
    astc_append = 0;
    /* bound in stacks; made whole, saying why */
    Surf e = tex(2048, 2048, 1, 12, 16, 3, 0, 1), s = tex(1536, 1536, 1, 11, 16, 3, 0, 1), n = t;
    e.f = s.f = t.f;
    e.f.bw = e.f.bh = 8;
    s.f.bw = s.f.bh = 6;
    e.tile_w = s.tile_w = 4;
    e.layer_size = e.size = bl_layer_size(2048, 2048, 1, &e.f, 3, 0, 4, 12);
    s.layer_size = s.size = bl_layer_size(1536, 1536, 1, &s.f, 3, 0, 4, 11);
    g_res.shape = (VkResidentShape){{256, 256, 1}, 4, 1};
    g_log[0] = 0;
    CHECK(resident_image(&e, &ii) && e.bricks == 85 && e.tail_first == 4 && e.brick_bytes == 0x4000 && !g_log[0]);
    u8 want[86] = {0};
    g_map.count = 1;
    g_map.gva[0] = GVA, g_map.n[0] = 0x10000; /* tile (0, 0) of level 0: its units 0 and 1, 32 KiB each */
    CHECK(resident_bind_mapped(&e, want) && g_res.pages == 86);
    for (u32 i = 0; i < 86; i++) CHECK(g_res.bound[i] == (i == 0 || i == 1 || i == 8 || i == 9));
    for (u32 i = 0; i < 86; i++) CHECK(!want[i] == (i > 3)); /* the two units' four shares, in the guest's order */
    g_map.count = 0;
    g_log[0] = 0;
    CHECK(!resident_image(&s, &ii) && !s.bricks);
    CHECK(strstr(g_log, "in ASTC 6x6 as BC7 with holes") && strstr(g_log, "256x256x1 texels, not whole 6x6 blocks,"));
    g_res.shape = (VkResidentShape){{0, 0, 0}, 0, 0};
    g_vk.resident = 1;
    g_vk.feat.sparseResidencyImage2D = VK_TRUE;
    n.bricks = 0;
    g_log[0] = 0;
    CHECK(!resident_image(&n, &ii) && !n.bricks);
    CHECK(strstr(g_log, "1024x1024x1 texture of 11 levels in ASTC 4x4 as BC7 with holes in its mapping is made whole: "
                        "the device has no sparse images of format 145"));
    g_vk.resident = 0;
    g_vk.feat.sparseResidencyImage2D = VK_FALSE;
    copy_bpp = 4, copy_bw = copy_bh = 1;
}

/* A texture with holes in its mapping, loaded a piece at a time, checked by its contents (where write tracking can't
   tell): 512x512 RGBA8, 1 MiB, two runs of 64 KiB mapped. Its hash reads those runs a piece of 4 KiB at a time, 32
   pieces, not its 1 MiB (256): unchanged, it loads nothing; a run mapped, a run unmapped, a byte of a run and a run
   moved with its bytes (the same runs in the same order, one at another place) change it; a byte where nothing is
   mapped (the stubs read guest memory there, a hole reads zeros) doesn't, nor its write-back by the GPU. Loaded whole,
   its hash reads all of it */
static void check_hash_runs(void) {
    Surf t = tex(512, 512, 1, 1, 4, 4, 0, 1);
    u8* moved = malloc(0x10000);
    u64 h0;
    CHECK(moved);
    memcpy(moved, guest + 0x20000, 0x10000);
    t.size = t.layer_size = bl_level_size(512, 512, 1, &t.f, 4, 0, 0, 0, NULL, NULL);
    CHECK(t.size == 1u << 20);
    t.piecewise = 1;
    t.upload_valid = 1;
    g_map.count = 2;
    g_map.gva[0] = GVA + 0x10000, g_map.n[0] = 0x10000;
    g_map.gva[1] = GVA + 0x80000, g_map.n[1] = 0x10000;
    copy_bpp = 4, copy_bw = copy_bh = 1;
    /* each check: the change before it, then whether it loads */
    const u32 rehash[9][2] = {{0, 1}, {0, 0}, {1, 1}, {2, 1}, {0, 0}, {3, 1}, {4, 0}, {5, 1}, {0, 0}};
    for (u32 i = 0; i < 9; i++) {
        if (rehash[i][0] == 1) g_map.count = 3, g_map.gva[2] = GVA + 0xC0000, g_map.n[2] = 0x10000; /* mapped */
        if (rehash[i][0] == 2) g_map.count = 2;                                                     /* unmapped */
        if (rehash[i][0] == 3) guest[0x80005] ^= 0x5A;                                              /* in a run */
        if (rehash[i][0] == 4) guest[0x40000] ^= 0x5A;                                              /* in a hole */
        if (rehash[i][0] == 5) { /* run 0's bytes, mapped at 0x20000 instead */
            memcpy(guest + 0x20000, guest + 0x10000, 0x10000);
            g_map.gva[0] = GVA + 0x20000;
        }
        stage_used = copied_used = nregs = 0;
        h0 = g_hashes;
        CHECK(tex_rehash(&t) == (int)rehash[i][1] && g_hashes - h0 == 16u * g_map.count);
    }
    vk_tex_note_writeback(&t); /* its memory written by the GPU: the hash it keeps is its runs' */
    stage_used = copied_used = nregs = 0;
    CHECK(tex_rehash(&t) == 0);
    guest[0x80005] ^= 0x5A;
    guest[0x40000] ^= 0x5A;
    memcpy(guest + 0x20000, moved, 0x10000);
    free(moved);
    t.piecewise = 0;
    stage_used = copied_used = nregs = 0;
    h0 = g_hashes;
    tex_rehash(&t);
    CHECK(g_hashes - h0 == 256);
    g_map.count = 0;
}

/* a 1024x1024 BC7 texture of 3 levels held as ETC2 RGBA8, blocks 1 GOB high: 65536, 16384 and 4096 blocks */
static Surf bc7_etc2(void) {
    Surf t = tex(1024, 1024, 1, 3, 16, 0, 0, 1);
    t.f = (VkFmt){.vk = VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, .bpp = 16, .bw = 4, .bh = 4, .conv = VKC_BC7};
    t.astc_later = 1;
    t.upload_valid = 1;
    t.size = 0;
    for (u32 l = 0; l < 3; l++) {
        u32 lbh, lbd;
        t.size += bl_level_size(1024, 1024, 1, &t.f, 0, 0, 0, l, &lbh, &lbd);
    }
    t.layer_size = t.size;
    return t;
}
/* n presents that sample t */
static void sampled(Surf* t, u32 n) {
    for (u32 i = 0; i < n; i++) {
        g_vk_present++;
        astc_later_read(t);
        astc_later_read(t); /* twice a present counts once */
    }
}
/* the regions copied since the counts were reset: all into img, each block the guest's at its place in the layout
   with the conversion's mark for the format; the blocks they hold */
static u32 bc7_regions(const Surf* t, VkImage img, VkFormat format) {
    u32 blocks = 0;
    for (u32 i = 0; i < nregs; i++) {
        const VkBufferImageCopy* r = &regs[i];
        const u32 l = r->imageSubresource.mipLevel, lw = 1024u >> l, cx = lw / 4;
        u64 at = 0;
        u32 lbh = 0, lbd = 0;
        for (u32 k = 0; k < l; k++) at += bl_level_size(1024, 1024, 1, &t->f, 0, 0, 0, k, &lbh, &lbd);
        bl_level_size(1024, 1024, 1, &t->f, 0, 0, 0, l, &lbh, &lbd);
        CHECK(regs_img[i] == img && r->imageOffset.x == 0 && r->imageExtent.width == lw && r->bufferRowLength == lw);
        for (u32 y = 0; y < r->imageExtent.height / 4; y++)
            for (u32 k = 0; k < cx * 16; k += 5) { /* every fifth byte: each block's, and each of its places in turn */
                const u32 by = (u32)r->imageOffset.y / 4 + y;
                const u8 want = guest[at + bl_offset(k, by, 0, cx * 16, lw / 4, lbh, 0)] ^ bc7_mark(format);
                CHECK(copied[r->bufferOffset + (u64)y * cx * 16 + k] == want);
            }
        blocks += cx * r->imageExtent.height / 4;
    }
    return blocks;
}
static void check_astc_between_frames(void) {
    const VkImage etc2 = (VkImage)(uintptr_t)0x1A6;
    VkAstcLaterStats st;
    vkGetPhysicalDeviceFormatProperties = no_format_properties;
    g_vk.alloc_cap = 256ull << 20;
    copy_bpp = 16, copy_bw = copy_bh = 4;
    /* every 16 bytes of its memory a BC7 block of mode 0, which holds no alpha: one alpha a block */
    fill();
    for (u32 i = 0; i < GUEST; i += 16) guest[i] |= 1;
    Surf t = bc7_etc2();
    /* sampled in one present short of VK_ASTC_LATER_READS: not looked at, and the frames after do nothing */
    sampled(&t, VK_ASTC_LATER_READS - 1);
    nregs = 0, g_submits = 0;
    vk_tex_later_frame();
    CHECK(!t.astc.queued && !t.astc_settled && t.astc_reads == VK_ASTC_LATER_READS - 1 && !nregs && !g_submits);
    /* a write (its pieces) counts its presents from none */
    astc_later_wrote(&t);
    sampled(&t, VK_ASTC_LATER_READS - 1);
    CHECK(!t.astc.queued && t.astc_reads == VK_ASTC_LATER_READS - 1);
    /* the next present queues it. No image for it yet: it waits, still ETC2 */
    sampled(&t, 1);
    vk_tex_astc_later_stats(&st);
    CHECK(t.astc.queued && st.waiting == 1 && !st.kept);
    vk_tex_later_frame();
    CHECK(t.astc.queued && !t.astc.img && !nregs && t.img == etc2);
    /* with one: 4096 blocks a frame, 16 rows of level 0's 256 (its layout's blocks are 8 rows high: rows of two of
       them), each frame its own submission; then level 1, 32 rows a frame of its 128, and level 2 whole. The blocks
       are the guest's, through the conversion's ASTC build, into the new image */
    g_images.ok = 1;
    u32 frames = 0, total = 0;
    while (t.astc.queued && frames < 32) {
        stage_used = copied_used = nregs = 0;
        g_conv_blocks = 0, g_submits = 0;
        vk_tex_later_frame();
        const VkImage making = (VkImage)(uintptr_t)0x1001;
        CHECK(g_images.made == 1 && g_images.format == VK_FORMAT_ASTC_4x4_UNORM_BLOCK && g_submits == 1);
        const u32 blocks = bc7_regions(&t, making, VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
        CHECK(blocks == g_conv_blocks && blocks == VK_ASTC_LATER_BLOCKS);
        /* the game's image is the ETC2 one until the last frame */
        CHECK(t.astc.queued ? t.img == etc2 && t.f.vk == VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK : t.img == making);
        total += blocks;
        frames++;
    }
    vk_tex_astc_later_stats(&st);
    CHECK(frames == 21 && total == 65536 + 16384 + 4096 && st.made == 1 && st.blocks == total && !st.waiting);
    CHECK(t.f.vk == VK_FORMAT_ASTC_4x4_UNORM_BLOCK && t.astc_settled && !t.astc.img);
    /* done with: more presents queue nothing */
    sampled(&t, 2 * VK_ASTC_LATER_READS);
    CHECK(!t.astc.queued);
    /* written whole now: an ETC2 RGBA8 image again, which the load fills as the first did, and its presents from none */
    stage_used = copied_used = nregs = 0;
    CHECK(tex_upload(&t) && t.f.vk == VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK && t.img == (VkImage)(uintptr_t)0x1002);
    CHECK(bc7_regions(&t, t.img, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK) == total && !t.astc_settled && !t.astc_reads);
    vk_tex_astc_later_stats(&st);
    CHECK(st.undone == 1);
    /* queued again, and written two frames into its encode: what was made goes, and it is out of the queue */
    sampled(&t, VK_ASTC_LATER_READS);
    vk_tex_later_frame();
    vk_tex_later_frame();
    CHECK(t.astc.queued && t.astc.img == (VkImage)(uintptr_t)0x1003 && t.astc.row == 32);
    stage_used = copied_used = nregs = 0;
    CHECK(tex_upload(&t) && !t.astc.queued && !t.astc.img && t.img == (VkImage)(uintptr_t)0x1002);
    vk_tex_astc_later_stats(&st);
    CHECK(st.restarted == 1 && !st.waiting && st.made == 1);
    nregs = 0;
    vk_tex_later_frame();
    CHECK(!nregs);
    /* one block whose alpha moves (mode 6, its two alpha ends apart), in level 1: looked at once, left as ETC2 */
    u32 lbh, lbd;
    const u64 level1 = bl_level_size(1024, 1024, 1, &t.f, 0, 0, 0, 0, &lbh, &lbd);
    u8* block = guest + level1 + 16 * 1000;
    u8 saved[16];
    memcpy(saved, block, 16);
    memset(block, 0, 16);
    block[0] = 0x40;
    block[7] = 0x7F; /* bits 56 to 62: the second alpha end */
    Surf moving = bc7_etc2();
    sampled(&moving, VK_ASTC_LATER_READS);
    vk_tex_astc_later_stats(&st);
    CHECK(!moving.astc.queued && moving.astc_settled && st.kept == 1 && !st.waiting);
    sampled(&moving, VK_ASTC_LATER_READS);
    vk_tex_astc_later_stats(&st);
    CHECK(!moving.astc.queued && st.kept == 1);
    /* written with one alpha a block again, it is looked at again and queued; freed, it leaves the queue */
    memcpy(block, saved, 16);
    astc_later_wrote(&moving);
    sampled(&moving, VK_ASTC_LATER_READS);
    CHECK(moving.astc.queued);
    vk_tex_later_frame();
    CHECK(moving.astc.img);
    vk_tex_later_forget(&moving);
    vk_tex_astc_later_stats(&st);
    CHECK(!moving.astc.queued && !moving.astc.img && !st.waiting);
    /* a texture that isn't such a one is never looked at */
    Surf plain = bc7_etc2();
    plain.astc_later = 0;
    sampled(&plain, 2 * VK_ASTC_LATER_READS);
    CHECK(!plain.astc.queued && !plain.astc_reads);
    g_images.ok = 0;
    copy_bpp = 4, copy_bw = copy_bh = 1;
    fill();
}
int main(void) {
    fill();
    check_take_skip();
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
    /* no strip asks the scratch buffer for more than a quarter of the cap, as the buffer stays as big as the most
       asked of it: a 128x128 RGBA8 level of 64 KiB, the cap, in 4 strips of 32 rows, each asking for its 16 KiB alone.
       The level went whole, asking for 64 bytes more than the cap: no buffer, and no upload */
    g_vk.alloc_cap = 64 << 10;
    {
        Surf q = tex(128, 128, 1, 1, 4, 0, 0, 1);
        const u64 quarter = (64 << 10) / VK_PART_SHARE;
        stage_used = copied_used = nregs = 0;
        copy_bpp = 4;
        g_scratch_most = 0;
        int ok = tex_upload(&q);
        if (!ok || nregs != 4 || g_scratch_most > quarter)
            fprintf(
                stderr,
                "a 128x128 RGBA8 level under a cap of 65536 bytes: upload %s in %u strips (4 expected), the scratch "
                "buffer asked for %llu bytes, %llu at most may stay\n",
                ok ? "done" : "failed", nregs, (unsigned long long)g_scratch_most, (unsigned long long)quarter);
        CHECK(ok && nregs == 4 && g_scratch_most <= quarter);
        upload_and_check(&q, GUEST, 4); /* every texel where the layout has it */
    }
    /* past a quarter of the allocation cap, strips: under 64 KiB a row of the picture's blocks (16 GOBs high,
       960 KiB) doesn't fit, so it goes a GOB row (8 rows, 7680 bytes each) at a time, each GOB of it gathered from its
       block, 17 of them; under 16 KiB the pitch-linear level in strips of 13 rows (its second level fits whole), and
       under 4 KiB the 3D level (blocks of 2 GOBs high and 2 slices deep, 4096 bytes a row of them) a row of blocks of
       2 slices at a time */
    g_vk.alloc_cap = (64 << 10) * VK_PART_SHARE;
    upload_and_check(&a, GUEST, 17);
    g_vk.alloc_cap = (16 << 10) * VK_PART_SHARE;
    Surf g = tex(300, 40, 1, 2, 4, 0, 0, 0);
    upload_and_check(&g, GUEST, 4);
    g_vk.alloc_cap = (4 << 10) * VK_PART_SHARE;
    upload_and_check(&c, GUEST, 4);
    g_vk.alloc_cap = (16 << 10) * VK_PART_SHARE;
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
    /* an ASTC array of 5 layers: a level at a time over all of them (one game's arrays of 118 layers took 667 ms of the
       GPU a layer at a time), each layer's block rows deswizzled from its own block linear surface, a layer_size
       after the one before */
    {
        Surf a = tex(60, 37, 1, 1, 16, 1, 0, 1);
        a.f.bw = a.f.bh = 4;
        a.f.conv = VKC_ASTC;
        a.layers = 5;
        a.layer_size = bl_layer_size(60, 37, 1, &a.f, 1, 0, 0, 1);
        a.size = a.layer_size * a.layers;
        stage_used = nregs = 0;
        nastc = nlayered = 0;
        astc_used = 0;
        CHECK(tex_upload(&a) && nregs == 0 && nlayered == 1 && nastc == 0);
        for (u32 layer = 0; layer < 5; layer++)
            for (u32 y = 0; y < 10; y++)
                for (u32 x = 0; x < 15 * 16; x++)
                    CHECK(astc_out[((layer * 10 + y) * 15) * 16 + x] ==
                          guest[layer * a.layer_size + bl_offset(x, y, 0, 15 * 16, 10, 1, 0)]);
        /* and of 2 levels: a call a level */
        Surf a2 = tex(60, 37, 1, 2, 16, 1, 0, 1);
        a2.f = a.f;
        a2.layers = 3;
        a2.layer_size = bl_layer_size(60, 37, 1, &a2.f, 1, 0, 0, 2);
        a2.size = a2.layer_size * a2.layers;
        stage_used = nregs = 0;
        nastc = nlayered = 0;
        astc_used = 0;
        CHECK(tex_upload(&a2) && nlayered == 2 && nastc == 0);
    }
    /* the BC7 cache's key: of an ASTC array held as BC7 straight, handed to vk_astc_upload_layers only where the cache
       takes the level (vk_astc_cached), the same for the same bytes, another for a byte of any layer changed, and none
       where the GPU holds bytes of it (an owner's pages hold another's bytes until they land); of one made BC7 between
       frames, kept in the texture for the encoder there, which the upload of its RGBA8 image doesn't take */
    {
        Surf b = tex(60, 37, 1, 1, 16, 1, 0, 1);
        b.f.bw = b.f.bh = 4;
        b.f.conv = VKC_ASTC;
        b.f.vk = VK_FORMAT_BC7_UNORM_BLOCK;
        b.layers = 5;
        b.layer_size = bl_layer_size(60, 37, 1, &b.f, 1, 0, 0, 1);
        b.size = b.layer_size * b.layers;
        u64 first[2];
        g_astc_key.takes = 0;
        stage_used = nregs = 0, astc_used = 0;
        CHECK(tex_upload(&b) && !g_astc_key.keyed);
        g_astc_key.takes = 1;
        stage_used = nregs = 0, astc_used = 0;
        CHECK(tex_upload(&b) && g_astc_key.keyed);
        memcpy(first, g_astc_key.content, sizeof first);
        stage_used = nregs = 0, astc_used = 0;
        CHECK(tex_upload(&b) && g_astc_key.keyed && !memcmp(first, g_astc_key.content, sizeof first));
        guest[3 * b.layer_size + 77] ^= 0x10;
        stage_used = nregs = 0, astc_used = 0;
        CHECK(tex_upload(&b) && g_astc_key.keyed && memcmp(first, g_astc_key.content, sizeof first));
        guest[3 * b.layer_size + 77] ^= 0x10;
        /* owners elsewhere, none on the level's pages: the same key */
        g_nown = 1;
        stage_used = nregs = 0, astc_used = 0;
        CHECK(tex_upload(&b) && g_astc_key.keyed && !memcmp(first, g_astc_key.content, sizeof first));
        g_astc_key.owner_over = 1;
        stage_used = nregs = 0, astc_used = 0;
        CHECK(tex_upload(&b) && !g_astc_key.keyed);
        g_nown = 0, g_astc_key.owner_over = 0;
        /* between frames: RGBA8 now, the key of each level kept, the same for the same bytes */
        Surf r = b;
        r.f.vk = VK_FORMAT_R8G8B8A8_UNORM;
        r.bc7_later = 1;
        stage_used = nregs = 0, astc_used = 0;
        CHECK(tex_upload(&r) && !g_astc_key.keyed && r.bc7.keyed == 1);
        u64 kept[2];
        memcpy(kept, r.bc7.key[0], sizeof kept);
        stage_used = nregs = 0, astc_used = 0;
        CHECK(tex_upload(&r) && r.bc7.keyed == 1 && !memcmp(kept, r.bc7.key[0], sizeof kept));
        guest[77] ^= 0x10;
        stage_used = nregs = 0, astc_used = 0;
        CHECK(tex_upload(&r) && r.bc7.keyed == 1 && memcmp(kept, r.bc7.key[0], sizeof kept));
        guest[77] ^= 0x10;
        g_astc_key.takes = 0;
        stage_used = nregs = 0, astc_used = 0;
        CHECK(tex_upload(&r) && r.bc7.keyed == 0);
        vk_tex_later_forget(&r);
    }
    /* a sparse texture's levels at least a tile in size have rows of whole tiles (tile_w). RGBA8 tiles of 128x128
       texels (8 GOBs across, blocks 16 high): 200 texels across (13 GOBs) take 16 at level 0, and levels 1 and 2 are
       narrower than a tile. Whole, in rows of blocks under 160 KiB (2 of them), and 8 GOB rows at a time under 64 KiB,
       each gathered from its blocks (4); the caps 4 times those, as a strip takes a quarter of one */
    Surf sp = tex(200, 256, 1, 3, 4, 4, 0, 1);
    sp.tile_w = 3;
    upload_and_check(&sp, GUEST, 1);
    g_vk.alloc_cap = (160 << 10) * VK_PART_SHARE;
    upload_and_check(&sp, GUEST, 2);
    g_vk.alloc_cap = (64 << 10) * VK_PART_SHARE;
    upload_and_check(&sp, GUEST, 4);
    /* a volume's: RGBA8 tiles of 32x32x16 (2 GOBs across, blocks 4 high and 16 deep). 80x64x32 (5 GOBs) takes 6, and
       level 1, 40x32x16, a tile high and deep, 4. Whole, in a row of blocks of a slab under 512 KiB (2), and 4 GOB rows
       of a slice under 64 KiB (64), the caps 4 times those */
    Surf sv = tex(80, 64, 32, 2, 4, 2, 4, 1);
    sv.tile_w = 1;
    g_vk.alloc_cap = 256ull << 20;
    upload_and_check(&sv, GUEST, 1);
    g_vk.alloc_cap = (512 << 10) * VK_PART_SHARE;
    upload_and_check(&sv, GUEST, 2);
    g_vk.alloc_cap = (64 << 10) * VK_PART_SHARE;
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
    /* the change check, mem_hash: the same hash whether the GPU holds bytes somewhere (own_any: the texture read
       through vk_guest_peek) or not, and a byte past a page's first 64 changes it, as a row the CPU rewrites does */
    {
        const u64 at = GVA + 0x20000, size = 4 * 4096;
        u64 plain = mem_hash(at, size), owned;
        g_nown = 1;
        owned = mem_hash(at, size);
        CHECK(owned == plain);
        guest[0x20000 + 4096 + 100] ^= 0x5A;
        CHECK(mem_hash(at, size) != owned);
        guest[0x20000 + 4096 + 100] ^= 0x5A;
        /* held whole by one GPU-owned range: its owner's identity, with no piece peeked; the same identity the same
           hash, another one another */
        g_whole_tag = 0x1234, g_peeks = 0;
        u64 whole = mem_hash(at, size);
        CHECK(g_peeks == 0 && whole != owned && mem_hash(at, size) == whole);
        g_whole_tag = 0x1235;
        CHECK(mem_hash(at, size) != whole && g_peeks == 0);
        g_whole_tag = 0;
        g_nown = 0;
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
    /* loaded a piece at a time: made with tiles 0 and 2 mapped, its image is zeros, then after a barrier those tiles'
       blocks (8 KiB each: a GOB across, 8 rows, 16 slices), 128 KiB read of its 1.6 MB. Then the CPU writing a page of
       tile 2 loads that page's block, and tile 1 mapped (a GPU-side write of its pages) loads its 8 blocks: 5 at the
       end of the first row of blocks and 3 at the start of the next. A check that finds nothing loads nothing */
    {
        Surf t = sparse_volume(1);
        u64 uploads, bytes, uploads2, bytes2;
        t.piecewise = 1;
        t.hash = 0;
        stage_used = copied_used = nregs = all_bars = 0;
        memset(model, 0xEE, sizeof model);
        vk_tex_stats(&uploads, &bytes);
        CHECK(tex_load_pieces(&t) && t.upload_valid);
        vk_tex_stats(&uploads2, &bytes2);
        CHECK(bytes2 - bytes == 2 * 0x10000 && uploads2 - uploads == 1);
        u32 zeros = 0;
        while (zeros < nregs && regs_bars[zeros] == 0) zeros++;
        CHECK(zeros && zeros < nregs);
        CHECK(nregs - zeros == 2); /* each tile a run of 8 blocks of one row of blocks: 8 GOBs, 128 texels across */
        for (u32 i = zeros; i < nregs; i++) CHECK(regs_bars[i] == 1 && regs[i].imageExtent.width == 128);
        model_apply();
        CHECK(model_wrong() == 0);
        const u32 x = 3 * 16 + 1, y = 9, z = 6; /* tile 2 */
        const u64 o = bl_offset(x * 4, y, z, 832, 64, 0, 4);
        guest[o] ^= 0x5A;
        g_tracking.stamp = ++g_tracking.clock;
        g_tracking.at = o, g_tracking.len = 1;
        g_vk_present++;
        g_tracking.epoch++;
        stage_used = copied_used = nregs = 0;
        vk_tex_stats(&uploads, &bytes);
        CHECK(tex_check(&t, t.gva) == &t);
        vk_tex_stats(&uploads2, &bytes2);
        CHECK(bytes2 - bytes == 8192 && nregs == 1);
        model_apply();
        CHECK(model_wrong() == 0);
        g_map.count = 3;
        g_map.gva[2] = GVA + 0x10000, g_map.n[2] = 0x10000;
        g_gpu_writes.gen = 1, g_gpu_writes.at = GVA + 0x10000, g_gpu_writes.len = 0x10000;
        g_vk_present++;
        stage_used = copied_used = nregs = 0;
        vk_tex_stats(&uploads, &bytes);
        CHECK(tex_check(&t, t.gva) == &t);
        vk_tex_stats(&uploads2, &bytes2);
        CHECK(bytes2 - bytes == 0x10000 && nregs == 2);
        model_apply();
        CHECK(model_wrong() == 0);
        g_vk_present++;
        g_tracking.epoch++;
        stage_used = copied_used = nregs = 0;
        CHECK(tex_check(&t, t.gva) == &t && nregs == 0);
        guest[o] ^= 0x5A;
        g_map.count = 0;
        memset(&g_tracking, 0, sizeof g_tracking);
        memset(&g_gpu_writes, 0, sizeof g_gpu_writes);
    }
    /* What a check takes in is marked with the clock's next value (buffer.c ww_take_scanned), so a written texture's
       baseline is the clock after its check. Taken before it, the next check takes the same write in again: a texture
       checked by its hash hashes it once more for nothing, a texture with holes in its mapping loads its blocks again */
    {
        Surf t = tex(8, 2, 1, 1, 4, 0, 0, 0);
        t.size = t.pitch * t.h;
        copy_bpp = 4;
        stage_used = copied_used = nregs = 0;
        CHECK(tex_upload(&t));
        t.hash = mem_hash(t.gva, t.size);
        g_tracking.on = 1;
        g_tracking.clock = t.wseen = 10;
        for (u32 round = 0; round < 2; round++) {
            guest[0] ^= 0x5A;
            g_tracking.pending = 1;
            g_tracking.at = 0, g_tracking.len = 1;
            g_vk_present++;
            g_tracking.epoch++;
            stage_used = copied_used = nregs = 0;
            CHECK(tex_check(&t, t.gva) == &t && nregs == 1 && copied[regs[0].bufferOffset] == guest[0]);
            g_vk_present++;
            g_tracking.epoch++;
            stage_used = copied_used = nregs = 0;
            g_hashes = 0;
            CHECK(tex_check(&t, t.gva) == &t && nregs == 0 && g_hashes == 0);
        }
        Surf v = sparse_volume(1);
        const u64 o = bl_offset((3 * 16 + 1) * 4, 9, 6, 832, 64, 0, 4); /* tile 2 */
        v.piecewise = 1;
        v.hash = 0;
        CHECK(tex_load_pieces(&v) && v.upload_valid);
        guest[o] ^= 0x5A;
        g_tracking.pending = 1;
        g_tracking.at = o, g_tracking.len = 1;
        g_vk_present++;
        g_tracking.epoch++;
        stage_used = copied_used = nregs = 0;
        CHECK(tex_check(&v, v.gva) == &v && nregs == 1);
        g_vk_present++;
        g_tracking.epoch++;
        stage_used = copied_used = nregs = 0;
        CHECK(tex_check(&v, v.gva) == &v && nregs == 0);
        guest[o] ^= 0x5A;
        g_map.count = 0;
        memset(&g_tracking, 0, sizeof g_tracking);
    }
    check_block_linear_changes();
    check_untracked_runs();
    check_pitch_linear_changes();
    check_astc_changes();
    check_resident_layers();
    check_resident_astc();
    check_hash_runs();
    check_astc_between_frames();
    printf("tex_upload_rows: %u checks passed\n", checks);
    return 0;
}
