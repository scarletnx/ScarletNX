/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* tex_upload (vk/texture.c) from guest memory through the staging buffer and the GPU's deswizzles and conversions
   (a CPU model of them below, run as they are recorded, which checks the barriers around them) to the copies into
   the image, each copy's bytes checked against a walk of the block-linear layout byte by byte (bl_offset below): a
   level whose rows are whole 16-byte pieces, one whose rows end inside a piece, a 3D level with blocks of slices, mip
   levels, a level read through gmem_read, a pitch-linear level, and a converted format. No level takes a buffer of
   its size on the CPU. Levels past the allocation cap go in strips, each region checked where it lands, and an ASTC
   level's block rows reach vk_astc_upload strip by strip, deswizzled */
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
/* no unified memory, so no texture checked on the GPU: its uploads are copies, with the gate closed */
int vk_texcheck_prepare(VkImageCreateInfo* ii, VkViewFormats* vf) {
    (void)ii, (void)vf;
    return 0;
}
int vk_texcheck_begin(Surf* t, int force) {
    (void)t, (void)force;
    return 0;
}
void vk_texcheck_end(void) {}
int vk_gate_open(void) { return 0; }
void vk_gate_copy(VkCommandBuffer cb, Surf* t, VkBuffer src, u32 n, const VkBufferImageCopy* r) {
    rec_CmdCopyBufferToImage(cb, src, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, r);
}
/* no unified memory: every level is staged, which the checks below read back */
int vk_uma_read(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off) {
    (void)gva, (void)len, (void)align, (void)buf, (void)off;
    return 0;
}
u8* gmem_host(u64 gva, u64 size) { return gmem_ptr(gva, size); }
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
void vk_render_end(void) {}
void vk_surf_use(VkCommandBuffer cb, Surf* s, VkImageLayout layout, VkPipelineStageFlags2 stage_,
                 VkAccessFlags2 access) {
    (void)cb, (void)s, (void)layout, (void)stage_, (void)access;
}
VkImageAspectFlags vk_aspect(const VkFmt* f) {
    return f->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}
/* the ASTC upload asks for its block rows in strips of 3: they land in astc_out, slice after slice */
static u8 astc_out[64 << 10];
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
void vk_bl_range(const VkBlMove* m, u64* lo, u64* hi) {
    u64 block_row = (u64)((m->row_bytes + 63) >> 6) << (9 + m->bh + m->bd);
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
                u64 o = bl_offset(x, m->y0 + y, m->z0 + z, m->row_bytes, m->height, m->bh, m->bd);
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
void vk_view_formats(VkImageCreateInfo* ii, VkViewFormats* vf) { (void)ii, (void)vf; }
static struct {
    int on;
    u32 clock, stamp, epoch;
} g_tracking;
int vk_written_since(const u8* host, u64 len, u32 since) {
    (void)host, (void)len;
    return g_tracking.on ? g_tracking.stamp > since : -1;
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
/* the block linear layout: GOBs of 64 bytes by 8 rows in blocks of 1 << bh by 1 << bd GOBs, the GOB's 16 byte groups
   in their order */
u64 bl_offset(u32 x, u32 y, u32 z, u32 wb, u32 h, u32 bh_log2, u32 bd_log2) {
    u32 gob_w = (wb + 63) / 64, bh = 1u << bh_log2, bd = 1u << bd_log2;
    u32 blocks_h = ((h + 7) / 8 + bh - 1) / bh, gy = y / 8, xb = x % 64, yb = y % 8;
    u64 block = 512ull * bh * bd;
    u64 o = ((u64)(z / bd) * blocks_h * gob_w + (u64)(gy / bh) * gob_w + x / 64) * block +
            ((u64)(z % bd) * bh + gy % bh) * 512;
    return o + (xb / 32) * 256 + (yb / 2) * 64 + ((xb % 32) / 16) * 32 + (yb % 2) * 16 + xb % 16;
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
        u32 rowb = lw * t->f.bpp, lbh = 0, lbd = 0, covered = 0, n = 0;
        u64 lsz = t->bl ? bl_level_size(t->w, t->h, d3, &t->f, t->bh, t->bd, l, &lbh, &lbd) : (u64)t->pitch * lh * ld;
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
                        u64 o = t->bl ? bl_offset(x, y, z, rowb, lh, lbh, lbd) : (u64)(z * lh + y) * t->pitch + x;
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
    printf("tex_upload_rows: %u checks passed\n", checks);
    return 0;
}
