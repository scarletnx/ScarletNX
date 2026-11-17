/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The barriers of an image's uses (gpu/vk/surface.c vk_surf_use), in the layouts that VK_LAYOUT names, with no
   device. Without unified image layouts, each use is in its own layout, and a change of layout records a barrier
   from the old layout to the new one. With them (g_vk.has_unified_layouts), each use is in GENERAL. The first read
   after a write records one barrier, GENERAL to GENERAL, that makes the write visible to all later reads
   (ALL_COMMANDS, MEMORY_READ). Later reads of another kind then record no barrier and do not end the pass. A write
   still records its barrier. A copy from one render target to another (rt_take_pixels) gives each image the layout
   that its barrier left it in. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma GCC diagnostic ignored "-Wmissing-field-initializers" /* surface.c builds without -Wextra */
#include "nx/gpu/vk/surface.c"

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ------------------------------------------------ what the uses record */
#define CMD ((VkCommandBuffer)(uintptr_t)0x1234)
typedef struct {
    VkImageMemoryBarrier2 barrier[8]; /* the image barriers recorded since the last clear */
    u32 barriers, ends;               /* barriers, and pass ends */
    VkImageLayout copy_src, copy_dst; /* the layouts that the last copy named */
    VkImage copy_src_img, copy_dst_img;
} Model;
static Model g_m;
static void clear(void) { g_m = (Model){0}; }
void rec_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* d) {
    CHECK(cb == CMD);
    CHECK(d->memoryBarrierCount == 0 && d->bufferMemoryBarrierCount == 0);
    for (u32 i = 0; i < d->imageMemoryBarrierCount; i++) {
        CHECK(g_m.barriers < 8);
        g_m.barrier[g_m.barriers++] = d->pImageMemoryBarriers[i];
    }
}
void vk_render_end_at(const char* file, int line) {
    (void)file, (void)line;
    g_m.ends++;
}
VkCommandBuffer vk_cmd(void) { return CMD; }
void rec_CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, u32 n,
                      const VkImageCopy* r) {
    (void)r;
    CHECK(cb == CMD && n == 1);
    g_m.copy_src_img = src;
    g_m.copy_dst_img = dst;
    g_m.copy_src = sl;
    g_m.copy_dst = dl;
}
void rec_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout dl, u32 n,
                              const VkBufferImageCopy* r) {
    (void)src, (void)r;
    CHECK(cb == CMD && n == 1);
    g_m.copy_dst_img = dst;
    g_m.copy_dst = dl;
}

/* ------------------------------------------------ what surface.c reaches otherwise: not used here */
#define STUB()                                                                                                         \
    do {                                                                                                               \
        fprintf(stderr, "FAIL: %s is not part of the model\n", __func__);                                              \
        exit(1);                                                                                                       \
    } while (0)
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(2);
}
void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }
#pragma GCC diagnostic ignored "-Wunused-parameter"
void bl_land(u8* dst, const u8* src, u32 row_bytes, u32 h, u32 d, u32 bh, u32 bd) { STUB(); }
u8* g_mem;
VkState g_vk;
u32 g_vk_present;
VkResourceStats g_vk_resource_stats;
u8* gmem_host(u64 gva, u64 size) { STUB(); }
u8* gmem_ptr(u64 gva, u64 size) { STUB(); }
int gmem_read(u64 gva, void* dst, u64 size) { STUB(); }
int gmem_write(u64 gva, const void* src, u64 size) { STUB(); }
void gmmu_aliases(u64 cpu, u64 size, void (*fn)(void* ctx, u64 gva, u64 n), void* ctx) { STUB(); }
u64 gmmu_run(u64 gva, u64 max, u64* cpu) { STUB(); }
u64 gmmu_translate(u64 gva) { STUB(); }
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz) { STUB(); }
u64 gpu_mem_generation(void) { STUB(); }
void gpu_mem_written(u64 gva, u64 size) { STUB(); }
int gpu_texture_live(u64 gva) { STUB(); }
u64 gpu_textures_added(void) { STUB(); }
u32 gpu_textures_ended(u64* out, u32 cap) { STUB(); }
int mem_lock_held(void) { STUB(); }
int mem_pool_offset(u64 a, u64* off) { STUB(); }
int own_covered(u64 addr, u64 size) { STUB(); }
int own_hash(u64 addr, u64 size, u64* h) { STUB(); }
u32 own_lookup(u64 addr, u64 size, OwnRange* out, u32 max) { STUB(); }
int own_owed_take(u64* addr, u64* size) { STUB(); }
int own_traps(void) { STUB(); }
void pend_add_va(PendPages* t, u64 gva, u64 size, int delta) { STUB(); }
int pend_hit_va(const PendPages* t, u64 gva, u64 size) { STUB(); }
u8* plat_alias_mirror(void) { STUB(); }
void rec_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* ri) { STUB(); }
void rec_CmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, u32 n,
                      const VkImageBlit* r, VkFilter filter) {
    STUB();
}
void rec_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, u32 n, const VkBufferCopy* r) { STUB(); }
void rec_CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkBuffer dst, u32 n,
                              const VkBufferImageCopy* r) {
    STUB();
}
void rec_CmdEndRendering(VkCommandBuffer cb) { STUB(); }
void rec_CmdResolveImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, u32 n,
                         const VkImageResolve* r) {
    STUB();
}
PFN_vkGetDeviceImageMemoryRequirements vkGetDeviceImageMemoryRequirements;
PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties;
PFN_vkGetPhysicalDeviceImageFormatProperties vkGetPhysicalDeviceImageFormatProperties;
void vk_all_barrier(VkCommandBuffer cb) { STUB(); }
void vk_astc_auto_trimmed(u64 usage, u64 left, u64 budget, int force) { STUB(); }
int vk_astc_bc7(void) { STUB(); }
void vk_astc_forget(const Surf* t, u32 level) { STUB(); }
u64 vk_astc_rgba8_bytes(void) { STUB(); }
void vk_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkPipelineStageFlags2 src_stage,
                VkAccessFlags2 src_access, VkImageLayout from, VkPipelineStageFlags2 dst_stage,
                VkAccessFlags2 dst_access, VkImageLayout to) {
    STUB();
}
int vk_bc1_tight(const char* why) { STUB(); }
void vk_bl_barrier(VkCommandBuffer cb, int before) { STUB(); }
int vk_bl_move(VkCommandBuffer cb, const VkBlMove* m, int to_bl, VkBuffer bl, u64 bl_at, VkBuffer lin, u64 lin_at) {
    STUB();
}
void vk_bl_range(const VkBlMove* m, u64* lo, u64* hi) { STUB(); }
VkBuffer vk_bl_scratch(u64 size, u64* cap) { STUB(); }
u64 vk_buf_reclaim(u32 before, u64 want) { STUB(); }
void vk_defer_bytes(u64 bytes) { STUB(); }
void vk_defer_image(VkImage img, DevMem a, VkImageView v1, VkImageView v2) { STUB(); }
u64 vk_deferred_bytes(void) { STUB(); }
int vk_depth_pack(VkCommandBuffer cb, const Surf* s, u32 layer, VkBuffer buf, u64 at, u32 pitch) { STUB(); }
int vk_depth_pack_mode(u32 zeta) { STUB(); }
const VkPhysicalDeviceMemoryProperties* vk_devmem_props(void) { STUB(); }
void vk_devmem_reserve_use(int on) { STUB(); }
void vk_finish_at(const char* file, int line) { STUB(); }
int vk_gfx_image_holds(const Surf* s) { STUB(); }
void vk_guest_read(u64 gva, u64 size, u8* host, VkBuffer buf, u64 at) { STUB(); }
int vk_guest_read_device(u64 gva, u64 size, VkBuffer buf, u64 at) { STUB(); }
u64 vk_hash(const void* p, u64 size) { STUB(); }
int vk_image_create(const VkImageCreateInfo* ii, VkImage* img, DevMem* mem) { STUB(); }
int vk_image_create_r(const VkImageCreateInfo* ii, VkImage* img, DevMem* mem, u32 owner, VkResult* res) { STUB(); }
VkResult vk_image_view_create(VkDevice device, const VkImageViewCreateInfo* info,
                              const VkAllocationCallbacks* allocation, VkImageView* view) {
    STUB();
}
void vk_lock(void) { STUB(); }
int vk_mem_heap(void) { STUB(); }
void vk_mem_use(u64* usage, u64* budget) { STUB(); }
int vk_ms_moves(const Surf* s) { STUB(); }
int vk_ms_pack(VkCommandBuffer cb, Surf* s, u32 layer, VkBuffer buf, u64 off, u32 pitch) { STUB(); }
int vk_ms_supported(VkFormat f, VkImageUsageFlags usage, VkImageCreateFlags flags, u32 msaa) { STUB(); }
VkImage vk_ms_unpack(VkCommandBuffer cb, Surf* s, u32 layer, u32 y0, u32 rows) { STUB(); }
int vk_ok_(VkResult r, const char* what, const char* file, int line) { STUB(); }
int vk_own_cpu_read(u64 cpu, u64 size, u32 presents) { STUB(); }
void vk_own_cpu_touched(u64 cpu, u64 size) { STUB(); }
void vk_own_publish(u64 cpu, u64 size, VkOwnRes* r, u64 off) { STUB(); }
VkOwnRes* vk_own_res_new(u64 size, u32 by) { STUB(); }
void vk_own_res_ref(VkOwnRes* r) { STUB(); }
void vk_own_res_unref(VkOwnRes* r) { STUB(); }
int vk_own_watch(u64 cpu, u64 size) { STUB(); }
u32 vk_present_before(u64 ago_ns, u32 presents) { STUB(); }
VkFormat vk_raw_format(u32 bpp) { STUB(); }
u8* vk_readback_buffer(u64 size, VkBuffer* buf) { STUB(); }
void vk_readback_ready(void) { STUB(); }
void vk_result_begin(void) { STUB(); }
void vk_result_end(void) { STUB(); }
VkFmt vk_rt_format(u32 fmt) { STUB(); }
VkFormat vk_srgb_as(VkFormat f, int srgb) { STUB(); }
u8* vk_stage(u64 size, u64 align, VkBuffer* buf, u64* off) { STUB(); }
u64 vk_stage_align(void) { STUB(); }
int vk_stage_device_at(const char* file, int line, u64 size, u64 align, VkBuffer* buf, u64* off) { STUB(); }
u8* vk_stage_guest_input(u64 gva, u64 source_size, u64 bytes, VkBuffer* buf, u64* off) { STUB(); }
int vk_tc_blit(VkCommandBuffer cb, u32 bpp, u32 fmt, u32 nb, u32 rows, VkBuffer in, u64 in_size, u64 src_at,
               u32 in_pitch, u64 col_at, u64 row_at, VkBuffer out, u64 out_at, u32 out_pitch) {
    STUB();
}
int vk_tex_can_coarsen(const Surf* t) { STUB(); }
u64 vk_tex_coarser(Surf* t) { STUB(); }
u64 vk_tex_demote(Surf* t) { STUB(); }
void vk_tex_evicted(const Surf* t) { STUB(); }
int vk_tex_finer(Surf* t) { STUB(); }
void vk_tex_later_forget(Surf* s) { STUB(); }
void vk_tex_note_writeback(Surf* s) { STUB(); }
int vk_tex_promote(Surf* t) { STUB(); }
int vk_uma_held(u64 cpu, u64 len) { STUB(); }
void vk_unlock(void) { STUB(); }
void vk_view_formats(VkImageCreateInfo* ii, VkViewFormats* vf, VkFormat raw) { STUB(); }
void vk_write_changed(u64 addr, const u8* res, const u8* org, u32 size) { STUB(); }
void vk_written_runs(const u8* host, u64 len, u32 since, void (*fn)(void*, u64, u64), void* user) { STUB(); }
int vk_written_since(const u8* host, u64 len, u32 since) { STUB(); }
void vk_ww_cool(const u8* host, u64 len) { STUB(); }
void vk_ww_landed(u64 cpu, u64 size, const u8* bytes) { STUB(); }
u32 vk_ww_now(void) { STUB(); }
VkFmt vk_zt_format(u32 fmt) { STUB(); }

/* ------------------------------------------------ the checks */
/* barrier k of the last uses: on s's image, from layout `from` to `to`, with the destination stage and access given */
static void want_barrier(u32 k, const Surf* s, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 stage,
                         VkAccessFlags2 access) {
    CHECK(k < g_m.barriers);
    const VkImageMemoryBarrier2* b = &g_m.barrier[k];
    if (b->image != s->img || b->oldLayout != from || b->newLayout != to || b->dstStageMask != stage ||
        b->dstAccessMask != access) {
        fprintf(stderr,
                "FAIL: unified %d, barrier %u: layout %d to %d, stage %llx, access %llx; want layout %d to %d, "
                "stage %llx, access %llx\n",
                g_vk.has_unified_layouts, k, b->oldLayout, b->newLayout, (unsigned long long)b->dstStageMask,
                (unsigned long long)b->dstAccessMask, from, to, (unsigned long long)stage, (unsigned long long)access);
        exit(1);
    }
}
static Surf surf(uintptr_t img) {
    Surf s = {0};
    s.img = (VkImage)img;
    s.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    s.f.vk = VK_FORMAT_R8G8B8A8_UNORM;
    s.f.bpp = 4;
    return s;
}
#define SAMPLE_STAGE  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
#define SAMPLE_ACCESS VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
#define COPY_STAGE    VK_PIPELINE_STAGE_2_COPY_BIT

/* a copy writes the image, a draw samples it, a copy reads it, a draw samples it again, and a copy writes it again */
static void uses(void) {
    int unified = g_vk.has_unified_layouts;
    VkImageLayout dst = unified ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkImageLayout src = unified ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkImageLayout read = unified ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    CHECK(VK_LAYOUT(TRANSFER_DST_OPTIMAL) == dst && VK_LAYOUT(TRANSFER_SRC_OPTIMAL) == src &&
          VK_LAYOUT(SHADER_READ_ONLY_OPTIMAL) == read);
    Surf s = surf(0x51);

    clear();
    vk_surf_use(CMD, &s, VK_LAYOUT(TRANSFER_DST_OPTIMAL), COPY_STAGE, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    CHECK(g_m.barriers == 1 && g_m.ends == 1);
    want_barrier(0, &s, VK_IMAGE_LAYOUT_UNDEFINED, dst, COPY_STAGE, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    CHECK(s.layout == dst);

    /* the first read after the write: in GENERAL, visible to every later read */
    clear();
    vk_surf_use(CMD, &s, VK_LAYOUT(SHADER_READ_ONLY_OPTIMAL), SAMPLE_STAGE, SAMPLE_ACCESS);
    CHECK(g_m.barriers == 1 && g_m.ends == 1);
    if (unified)
        want_barrier(0, &s, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_MEMORY_READ_BIT);
    else
        want_barrier(0, &s, dst, read, SAMPLE_STAGE, SAMPLE_ACCESS);
    CHECK(s.layout == read);

    /* reads of another kind: in GENERAL no barrier and no pass end, else each a change of layout */
    clear();
    vk_surf_use(CMD, &s, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), COPY_STAGE, VK_ACCESS_2_TRANSFER_READ_BIT);
    if (unified)
        CHECK(g_m.barriers == 0 && g_m.ends == 0);
    else {
        CHECK(g_m.barriers == 1 && g_m.ends == 1);
        want_barrier(0, &s, read, src, COPY_STAGE, VK_ACCESS_2_TRANSFER_READ_BIT);
    }
    CHECK(s.layout == src);
    clear();
    vk_surf_use(CMD, &s, VK_LAYOUT(SHADER_READ_ONLY_OPTIMAL), SAMPLE_STAGE, SAMPLE_ACCESS);
    if (unified)
        CHECK(g_m.barriers == 0 && g_m.ends == 0);
    else {
        CHECK(g_m.barriers == 1 && g_m.ends == 1);
        want_barrier(0, &s, src, read, SAMPLE_STAGE, SAMPLE_ACCESS);
    }
    CHECK(s.layout == read);

    /* a write after the reads takes its barrier in both */
    clear();
    vk_surf_use(CMD, &s, VK_LAYOUT(TRANSFER_DST_OPTIMAL), COPY_STAGE, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    CHECK(g_m.barriers == 1 && g_m.ends == 1);
    want_barrier(0, &s, read, dst, COPY_STAGE, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    CHECK(s.layout == dst);
}

/* a render target takes the pixels of another of its shape: the copy names the layouts that the barriers left */
static void take_pixels(void) {
    VkImageLayout want_src = VK_LAYOUT(TRANSFER_SRC_OPTIMAL), want_dst = VK_LAYOUT(TRANSFER_DST_OPTIMAL);
    Surf from = surf(0xf0), to = surf(0x70);
    from.is_rt = to.is_rt = 1;
    from.f.raw = to.f.raw = 1;
    from.w = to.w = from.sw = to.sw = 64;
    from.h = to.h = from.sh = to.sh = 32;
    from.layers = to.layers = 1;
    from.layer_size = to.layer_size = 64 * 32 * 4;
    from.wseq = 2;
    to.wseq = 1;
    /* a target that draws already noted in this present: vk_rt_drawn keeps its baseline and its lists */
    to.draw_wseen = 1;
    to.drawn_at = g_vk_present;
    to.in_drawn = to.in_linear = 1;
    vk_surf_use(CMD, &from, VK_LAYOUT(COLOR_ATTACHMENT_OPTIMAL), VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    clear();
    CHECK(rt_take_pixels(&to, &from, 1) == 1);
    CHECK(g_m.copy_src_img == from.img && g_m.copy_dst_img == to.img);
    CHECK(from.layout == want_src && to.layout == want_dst);
    CHECK(g_m.copy_src == want_src && g_m.copy_dst == want_dst);
    CHECK(g_m.barriers == 2);
    /* the first read after the draw's write: in GENERAL, for every later read */
    if (g_vk.has_unified_layouts)
        want_barrier(0, &from, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_MEMORY_READ_BIT);
    else
        want_barrier(0, &from, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     COPY_STAGE, VK_ACCESS_2_TRANSFER_READ_BIT);
    want_barrier(1, &to, VK_IMAGE_LAYOUT_UNDEFINED, want_dst, COPY_STAGE, VK_ACCESS_2_TRANSFER_WRITE_BIT);
}

int main(void) {
    for (int unified = 0; unified < 2; unified++) {
        g_vk.has_unified_layouts = unified;
        uses();
        take_pixels();
    }
    puts("vk_surf_layouts: ok");
    return 0;
}
