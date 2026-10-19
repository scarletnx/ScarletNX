/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vk_surf_evict (vk/surface.c): a render target unused for ten seconds goes, and what it drew goes to guest memory
   first, as on the console, where draws land in memory: Mario Kart 8 Deluxe draws its Mii icons at boot and makes
   textures of that memory at its Mii selection, a minute later. Not a target already written back, one whose memory
   the game wrote after the draw, nor a parked one (its address is the newer target's), and not one never drawn; of two
   that overlap, the one drawn last is what memory keeps; those of one sweep wait for the GPU once. A drawn target
   memory can't take (a depth one, or a format that isn't the guest's bytes) stays: Mario Kart 8 Deluxe draws depth
   targets as a course loads and reads them through the race. The GPU side is stubbed: an image's pixels read back as
   the low byte of its handle */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma GCC diagnostic ignored "-Wmissing-field-initializers" /* surface.c builds without -Wextra */
#include "nx/gpu/vk/surface.c"

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ------------------------------------------------ guest memory: GPU addresses from GVA map to g_mem from 0 */
#define GVA   0x500000000ull
#define GUEST (1u << 20)
u8* g_mem;
u64 gmmu_translate(u64 gva) { return gva >= GVA && gva < GVA + GUEST ? gva - GVA + 0x1000 : 0; }
u8* gmem_ptr_untracked(u64 gva, u64 size) {
    return gmmu_translate(gva) && gmmu_translate(gva + size - 1) ? GP(gmmu_translate(gva)) : NULL;
}
u8* gmem_ptr(u64 gva, u64 size) { return gmem_ptr_untracked(gva, size); }
/* as gpu.c's: an address with no memory reads as zeros, and takes no writes */
int gmem_read(u64 gva, void* dst, u64 size) {
    u8* m = gmem_ptr(gva, size);
    if (m)
        memcpy(dst, m, size);
    else
        memset(dst, 0, size);
    return 0;
}
int gmem_write(u64 gva, const void* src, u64 size) {
    u8* m = gmem_ptr(gva, size);
    if (m) memcpy(m, src, size);
    return 0;
}
static int g_cpu_wrote; /* what write tracking answers: the game wrote the target's memory after its draw */
static u32 g_ww = 1;
int vk_written_since(const u8* host, u64 len, u32 since) {
    (void)host, (void)len, (void)since;
    return g_cpu_wrote;
}
u32 vk_ww_now(void) { return g_ww++; }
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz) {
    (void)since, (void)a, (void)sz;
    return 0;
}
u64 gpu_mem_generation(void) { return 7; }
void gpu_mem_written(u64 gva, u64 size) { (void)gva, (void)size; }
u64 bl_offset(u32 x_bytes, u32 y, u32 z, u32 width_bytes, u32 height, u32 bh_log2, u32 bd_log2) {
    (void)x_bytes, (void)y, (void)z, (void)width_bytes, (void)height, (void)bh_log2, (void)bd_log2;
    return 0;
}
void deswizzle(u8* dst, u32 dst_pitch, const u8* src, u32 w_bytes, u32 h, u32 bh_log2) {
    (void)dst, (void)dst_pitch, (void)src, (void)w_bytes, (void)h, (void)bh_log2;
}
void swizzle(u8* dst, const u8* src, u32 src_pitch, u32 w_bytes, u32 h, u32 bh_log2) {
    (void)dst, (void)src, (void)src_pitch, (void)w_bytes, (void)h, (void)bh_log2;
}

/* ------------------------------------------------ the GPU side */
VkState g_vk;
u32 g_vk_present;
static uintptr_t g_next_image = 0x40;
static u8 g_readback[GUEST];
static u32 g_readbacks;
int vk_image_create(const VkImageCreateInfo* ii, VkImage* img, VmaAllocation* mem) {
    (void)ii;
    *img = (VkImage)g_next_image++;
    *mem = (VmaAllocation)(uintptr_t)1;
    return 1;
}
static void VKAPI_CALL copy_image_to_buffer(VkCommandBuffer cb, VkImage img, VkImageLayout l, VkBuffer b, uint32_t n,
                                            const VkBufferImageCopy* r) {
    (void)cb, (void)l, (void)b;
    for (u32 i = 0; i < n; i++)
        memset(g_readback + r[i].bufferOffset, (int)(u8)(uintptr_t)img,
               (size_t)r[i].imageExtent.width * r[i].imageExtent.height * 4);
    g_readbacks++;
}
static void VKAPI_CALL pipeline_barrier2(VkCommandBuffer cb, const VkDependencyInfo* d) { (void)cb, (void)d; }
static void VKAPI_CALL copy_buffer_to_image(VkCommandBuffer cb, VkBuffer b, VkImage img, VkImageLayout l, uint32_t n,
                                            const VkBufferImageCopy* r) {
    (void)cb, (void)b, (void)img, (void)l, (void)n, (void)r;
}
PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer = copy_image_to_buffer;
PFN_vkCmdPipelineBarrier2 vkCmdPipelineBarrier2 = pipeline_barrier2;
PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage = copy_buffer_to_image;
static VkResult VKAPI_CALL create_image_view(VkDevice d, const VkImageViewCreateInfo* ci,
                                             const VkAllocationCallbacks* a, VkImageView* v) {
    (void)d, (void)ci, (void)a;
    *v = (VkImageView)(uintptr_t)0x77;
    return VK_SUCCESS;
}
static void VKAPI_CALL format_properties(VkPhysicalDevice p, VkFormat f, VkFormatProperties* props) {
    (void)p, (void)f;
    memset(props, 0, sizeof *props);
}
PFN_vkCmdBlitImage vkCmdBlitImage;
PFN_vkCreateImageView vkCreateImageView = create_image_view;
PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties = format_properties;
u8* vk_readback_buffer(u64 size, VkBuffer* buf) {
    *buf = (VkBuffer)(uintptr_t)0xBB;
    return size <= sizeof g_readback ? g_readback : NULL;
}
void vk_readback_ready(void) {}
static u8 g_stage[GUEST];
u8* vk_stage(u64 size, u64 align, VkBuffer* buf, u64* off) {
    (void)align;
    *buf = (VkBuffer)(uintptr_t)0x57;
    *off = 0;
    return size <= sizeof g_stage ? g_stage : NULL;
}
void vk_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkPipelineStageFlags2 src_stage,
                VkAccessFlags2 src_access, VkImageLayout old_layout, VkPipelineStageFlags2 dst_stage,
                VkAccessFlags2 dst_access, VkImageLayout new_layout) {
    (void)cb, (void)img, (void)aspect, (void)src_stage, (void)src_access, (void)old_layout, (void)dst_stage,
        (void)dst_access, (void)new_layout;
}
VkCommandBuffer vk_cmd(void) { return (VkCommandBuffer)(uintptr_t)0xCB; }
void vk_render_end(void) {}
static u32 g_finishes; /* waits for the GPU */
void vk_finish(void) { g_finishes++; }
void vk_lock(void) {}
void vk_unlock(void) {}
void vk_result_begin(void) {}
void vk_result_end(void) {}
void vk_gfx_image_land(void) {}
void vk_defer_image(VkImage img, VmaAllocation a, VkImageView v1, VkImageView v2) {
    (void)img, (void)a, (void)v1, (void)v2;
}
void vk_tex_note_writeback(Surf* s) { (void)s; }
void vk_write_changed(u64 addr, const u8* res, const u8* org, u32 size) {
    (void)addr, (void)res, (void)org, (void)size;
}
void vk_view_formats(VkImageCreateInfo* ii, VkViewFormats* vf) { (void)ii, (void)vf; }
VkFormat vk_srgb_as(VkFormat f, int srgb) {
    (void)srgb;
    return f;
}
u64 vk_hash(const void* p, u64 size) {
    (void)p;
    return size;
}
static const VkFmt rgba8 = {.vk = VK_FORMAT_R8G8B8A8_UNORM, .bpp = 4, .bw = 1, .bh = 1, .raw = 1};
/* a color format whose Vulkan bytes aren't the guest's, as the packed float ones */
#define NOT_RAW 0xe0
static const VkFmt packed = {.vk = VK_FORMAT_B10G11R11_UFLOAT_PACK32, .bpp = 4, .bw = 1, .bh = 1};
VkFmt vk_rt_format(u32 fmt) { return fmt == NOT_RAW ? packed : rgba8; }
VkFmt vk_zt_format(u32 fmt) {
    VkFmt f = {.vk = VK_FORMAT_D32_SFLOAT, .bpp = 4, .bw = 1, .bh = 1, .depth = 1};
    (void)fmt;
    return f;
}
u64 plat_ns(void) { return 0; }
static unsigned g_went; /* the targets the last eviction log line says went to memory, or 0 */
void logf_(int lvl, const char* fmt, ...) {
    char line[256];
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (strstr(line, "went to guest memory")) g_went = (unsigned)strtoul(line + strlen("vulkan: "), NULL, 10);
}
int vk_ok_(VkResult r, const char* what, const char* file, int line) {
    (void)what, (void)file, (void)line;
    return r == VK_SUCCESS;
}
void fatal(const char* fmt, ...) {
    (void)fmt;
    exit(2);
}

/* ------------------------------------------------ the targets */
#define W 16
#define H 16
/* a pitch-linear RGBA8 target of w x h at a, drawn once in this present (as render.c's target_written does) */
static Surf* drawn(u64 a, u32 w, u32 h) {
    Surf* s = vk_rt(a, w, h, 0xd5, 0x1000, 0, w * 4);
    CHECK(s != NULL);
    s->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    s->wgen++;
    vk_rt_drawn(s);
    return s;
}
static int filled(u64 a, u32 bytes, u8 v) {
    const u8* m = gmem_ptr(a, bytes);
    for (u32 i = 0; i < bytes; i++)
        if (m[i] != v) return 0;
    return 1;
}
/* ten seconds and a sweep later: everything not used since present 0 goes */
static void ten_seconds(void) {
    g_vk_present = 600;
    g_went = 0;
    vk_surf_evict(1);
}

int main(void) {
    g_mem = calloc(1, GUEST + 0x2000);
    CHECK(g_mem != NULL);
    /* drawn and never written back: its pixels (the image's handle byte) are in memory once it is gone */
    g_vk_present = 0;
    Surf* icon = drawn(GVA, W, H);
    u8 px = (u8)(uintptr_t)icon->img;
    CHECK(filled(GVA, W * H * 4, 0));
    ten_seconds();
    CHECK(vk_rt_find(GVA) == NULL);
    CHECK(filled(GVA, W * H * 4, px));
    CHECK(g_readbacks == 1 && g_went == 1);
    /* written back already (a DMA copy flushed it): not read back again */
    g_vk_present = 0;
    Surf* flushed = drawn(GVA + 0x10000, W, H);
    vk_rt_flush(GVA + 0x10000);
    CHECK(g_readbacks == 2 && flushed->mem_wgen == flushed->wgen);
    ten_seconds();
    CHECK(g_readbacks == 2 && g_went == 0);
    /* the game wrote its memory after the draw: that memory stays */
    g_vk_present = 0;
    drawn(GVA + 0x20000, W, H);
    memset(gmem_ptr(GVA + 0x20000, W * H * 4), 0x33, W * H * 4);
    g_cpu_wrote = 1;
    ten_seconds();
    g_cpu_wrote = 0;
    CHECK(filled(GVA + 0x20000, W * H * 4, 0x33));
    CHECK(g_readbacks == 2 && g_went == 0);
    /* never drawn: nothing to write */
    g_vk_present = 0;
    CHECK(vk_rt(GVA + 0x30000, W, H, 0xd5, 0x1000, 0, W * 4) != NULL);
    ten_seconds();
    CHECK(filled(GVA + 0x30000, W * H * 4, 0) && g_readbacks == 2);
    /* two targets at one address: the parked one (8x8) isn't written over the one that took its place */
    g_vk_present = 0;
    Surf* old = drawn(GVA + 0x40000, 8, 8);
    Surf* now = drawn(GVA + 0x40000, W, H);
    CHECK(old != now && (old->gva >> 63) && vk_rt_find(GVA + 0x40000) == now);
    px = (u8)(uintptr_t)now->img; /* taken before the sweep frees the targets */
    ten_seconds();
    CHECK(filled(GVA + 0x40000, W * H * 4, px) && g_readbacks == 3 && g_went == 1);
    /* two that overlap, the one at the lower address drawn last: its pixels are what memory keeps where they meet,
       whichever the table holds first (its bucket comes first here, so the sweep meets it first) */
    g_vk_present = 0;
    Surf* upper = drawn(GVA + 0x50200, W, H);
    Surf* lower = drawn(GVA + 0x50000, W, H);
    CHECK(hkey(lower->gva) < hkey(upper->gva));
    u8 lower_px = (u8)(uintptr_t)lower->img, upper_px = (u8)(uintptr_t)upper->img;
    ten_seconds();
    CHECK(filled(GVA + 0x50000, W * H * 4, lower_px));
    CHECK(filled(GVA + 0x50400, 0x200, upper_px) && g_went == 2);
    /* three that go in one sweep: read back behind one wait for the GPU, each into its own memory */
    g_vk_present = 0;
    u8 three[3];
    for (int i = 0; i < 3; i++) three[i] = (u8)(uintptr_t)drawn(GVA + 0x60000 + (u64)i * 0x1000, W, H)->img;
    u32 finishes = g_finishes, readbacks = g_readbacks;
    ten_seconds();
    CHECK(g_finishes == finishes + 1 && g_readbacks == readbacks + 3 && g_went == 3);
    for (int i = 0; i < 3; i++) CHECK(filled(GVA + 0x60000 + (u64)i * 0x1000, W * H * 4, three[i]));
    /* a drawn depth target: memory can't take its pixels, so it stays, and memory is left as it was */
    g_vk_present = 0;
    Surf* z = vk_rt(GVA + 0x70000, W, H, 0x13, 0x1000, 1, W * 4);
    CHECK(z != NULL && z->f.depth);
    z->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    ten_seconds();
    CHECK(vk_rt_find(GVA + 0x70000) == z && filled(GVA + 0x70000, W * H * 4, 0));
    /* one never drawn, its layout undefined: nothing to lose, it goes */
    g_vk_present = 0;
    CHECK(vk_rt(GVA + 0x78000, W, H, 0x13, 0x1000, 1, W * 4) != NULL);
    ten_seconds();
    CHECK(vk_rt_find(GVA + 0x78000) == NULL);
    /* a parked depth target goes: its address is the newer one's, which stays */
    g_vk_present = 0;
    Surf* zold = vk_rt(GVA + 0x7c000, 8, 8, 0x13, 0x1000, 1, 8 * 4);
    zold->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    Surf* znow = vk_rt(GVA + 0x7c000, W, H, 0x13, 0x1000, 1, W * 4);
    znow->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    CHECK(zold != znow && (zold->gva >> 63));
    ten_seconds();
    CHECK(vk_rt_find(GVA + 0x7c000) == znow);
    for (Surf* s = vk_surf_first(GVA + 0x7c000); s; s = s->next) CHECK(s->gva != ((GVA + 0x7c000) | 1ull << 63));
    /* a drawn color target in a format memory can't take as is: it stays too */
    g_vk_present = 0;
    Surf* hdr = vk_rt(GVA + 0x80000, W, H, NOT_RAW, 0x1000, 0, W * 4);
    CHECK(hdr != NULL && !hdr->f.raw);
    hdr->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    hdr->wgen++;
    vk_rt_drawn(hdr);
    ten_seconds();
    CHECK(vk_rt_find(GVA + 0x80000) == hdr && filled(GVA + 0x80000, W * H * 4, 0));
    free(g_mem);
    printf("rt_evict: %u checks passed\n", checks);
    return 0;
}
