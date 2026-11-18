/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The trim of the renderer's caches (gpu/vk/surface.c vk_mem_trim) and the render targets that later ones drew all
   over (rt_superseded). Such a target holds no one's pixels, and a trim takes it before anything else. Where the
   driver moves memory by priority (g_vk.has_pageable), a trim for headroom leaves it for a second and eight presents,
   as the sweep every 30 presents does: targets have the highest priority, and the driver moves textures first. A trim
   for an allocation (force), or one where the driver does not move memory, takes it after two presents. No Vulkan
   loader, device or GPU is used: the rest of the renderer is stubs, and a call that the trim must not make aborts */
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

static _Noreturn void stub_hit(const char* name) {
    fprintf(stderr, "vk_mem_trim: an unexpected call of %s\n", name);
    abort();
}
#pragma GCC diagnostic ignored "-Wunused-parameter"
/* the rest of the renderer, guest memory and the platform: the trim calls none of these */
void bl_land(u8* dst, const u8* src, u32 row_bytes, u32 h, u32 d, u32 bh, u32 bd) { stub_hit("bl_land"); }
void fatal(const char* fmt, ...) { stub_hit("fatal"); }
u8* g_mem;
VkState g_vk;
u32 g_vk_present;
VkResourceStats g_vk_resource_stats;
u8* gmem_host(u64 gva, u64 size) { stub_hit("gmem_host"); }
u8* gmem_ptr(u64 gva, u64 size) { stub_hit("gmem_ptr"); }
int gmem_read(u64 gva, void* dst, u64 size) { stub_hit("gmem_read"); }
int gmem_write(u64 gva, const void* src, u64 size) { stub_hit("gmem_write"); }
void gmmu_aliases(u64 cpu, u64 size, void (*fn)(void* ctx, u64 gva, u64 n), void* ctx) { stub_hit("gmmu_aliases"); }
u64 gmmu_run(u64 gva, u64 max, u64* cpu) { stub_hit("gmmu_run"); }
u64 gmmu_translate(u64 gva) { stub_hit("gmmu_translate"); }
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz) { stub_hit("gpu_mem_dirty_since"); }
u64 gpu_mem_generation(void) { stub_hit("gpu_mem_generation"); }
void gpu_mem_written(u64 gva, u64 size) { stub_hit("gpu_mem_written"); }
int gpu_texture_live(u64 gva) { stub_hit("gpu_texture_live"); }
u64 gpu_textures_added(void) { stub_hit("gpu_textures_added"); }
int own_covered(u64 addr, u64 size) { stub_hit("own_covered"); }
int own_hash(u64 addr, u64 size, u64* h) { stub_hit("own_hash"); }
u32 own_lookup(u64 addr, u64 size, OwnRange* out, u32 max) { stub_hit("own_lookup"); }
int own_owed_take(u64* addr, u64* size) { stub_hit("own_owed_take"); }
int own_traps(void) { stub_hit("own_traps"); }
int pend_hit_va(const PendPages* t, u64 gva, u64 size) { stub_hit("pend_hit_va"); }
u8* plat_alias_mirror(void) { stub_hit("plat_alias_mirror"); }
void rec_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* ri) { stub_hit("rec_CmdBeginRendering"); }
void rec_CmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, u32 n,
                      const VkImageBlit* r, VkFilter filter) {
    stub_hit("rec_CmdBlitImage");
}
void rec_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, u32 n, const VkBufferCopy* r) {
    stub_hit("rec_CmdCopyBuffer");
}
void rec_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout dl, u32 n,
                              const VkBufferImageCopy* r) {
    stub_hit("rec_CmdCopyBufferToImage");
}
void rec_CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, u32 n,
                      const VkImageCopy* r) {
    stub_hit("rec_CmdCopyImage");
}
void rec_CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkBuffer dst, u32 n,
                              const VkBufferImageCopy* r) {
    stub_hit("rec_CmdCopyImageToBuffer");
}
void rec_CmdEndRendering(VkCommandBuffer cb) { stub_hit("rec_CmdEndRendering"); }
void rec_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* d) { stub_hit("rec_CmdPipelineBarrier2"); }
void rec_CmdResolveImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, u32 n,
                         const VkImageResolve* r) {
    stub_hit("rec_CmdResolveImage");
}
PFN_vkGetDeviceImageMemoryRequirements vkGetDeviceImageMemoryRequirements;
PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties;
PFN_vkGetPhysicalDeviceImageFormatProperties vkGetPhysicalDeviceImageFormatProperties;
void vk_all_barrier(VkCommandBuffer cb) { stub_hit("vk_all_barrier"); }
void vk_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkPipelineStageFlags2 src_stage,
                VkAccessFlags2 src_access, VkImageLayout from, VkPipelineStageFlags2 dst_stage,
                VkAccessFlags2 dst_access, VkImageLayout to) {
    stub_hit("vk_barrier");
}
void vk_bl_barrier(VkCommandBuffer cb, int before) { stub_hit("vk_bl_barrier"); }
int vk_bl_move(VkCommandBuffer cb, const VkBlMove* m, int to_bl, VkBuffer bl, u64 bl_at, VkBuffer lin, u64 lin_at) {
    stub_hit("vk_bl_move");
}
void vk_bl_range(const VkBlMove* m, u64* lo, u64* hi) { stub_hit("vk_bl_range"); }
VkBuffer vk_bl_scratch(u64 size, u64* cap) { stub_hit("vk_bl_scratch"); }
VkCommandBuffer vk_cmd(void) { stub_hit("vk_cmd"); }
void vk_defer_moved(u64 bytes) { stub_hit("vk_defer_moved"); }
int vk_depth_pack(VkCommandBuffer cb, const Surf* s, u32 layer, VkBuffer buf, u64 at, u32 pitch) {
    stub_hit("vk_depth_pack");
}
int vk_depth_pack_mode(u32 zeta) { stub_hit("vk_depth_pack_mode"); }
int vk_devmem_emptying(void) { stub_hit("vk_devmem_emptying"); }
VkResult vk_devmem_image(const VkImageCreateInfo* ici, const DevMemWant* want, VkImage* img, DevMem* mem) {
    stub_hit("vk_devmem_image");
}
void vk_devmem_info(DevMem mem, DevMemInfo* info) { stub_hit("vk_devmem_info"); }
void vk_devmem_owner(DevMem mem, void* owner) { stub_hit("vk_devmem_owner"); }
const VkPhysicalDeviceMemoryProperties* vk_devmem_props(void) { stub_hit("vk_devmem_props"); }
void vk_finish_at(const char* file, int line) { stub_hit("vk_finish_at"); }
void vk_guest_read(u64 gva, u64 size, u8* host, VkBuffer buf, u64 at) { stub_hit("vk_guest_read"); }
int vk_guest_read_device(u64 gva, u64 size, VkBuffer buf, u64 at) { stub_hit("vk_guest_read_device"); }
u64 vk_hash(const void* p, u64 size) { stub_hit("vk_hash"); }
int vk_image_create(const VkImageCreateInfo* ii, VkImage* img, DevMem* mem) { stub_hit("vk_image_create"); }
int vk_image_create_r(const VkImageCreateInfo* ii, VkImage* img, DevMem* mem, u32 owner, VkResult* res) {
    stub_hit("vk_image_create_r");
}
VkResult vk_image_view_create(VkDevice device, const VkImageViewCreateInfo* info,
                              const VkAllocationCallbacks* allocation, VkImageView* view) {
    stub_hit("vk_image_view_create");
}
void vk_lock(void) { stub_hit("vk_lock"); }
int vk_mem_heap(void) { stub_hit("vk_mem_heap"); }
void vk_mem_track(DevMem a, u32 owner) { stub_hit("vk_mem_track"); }
int vk_ms_moves(const Surf* s) { stub_hit("vk_ms_moves"); }
int vk_ms_pack(VkCommandBuffer cb, Surf* s, u32 layer, VkBuffer buf, u64 off, u32 pitch) { stub_hit("vk_ms_pack"); }
int vk_ms_supported(VkFormat f, VkImageUsageFlags usage, VkImageCreateFlags flags, u32 msaa) {
    stub_hit("vk_ms_supported");
}
VkImage vk_ms_unpack(VkCommandBuffer cb, Surf* s, u32 layer, u32 y0, u32 rows) { stub_hit("vk_ms_unpack"); }
int vk_ok_(VkResult r, const char* what, const char* file, int line) { stub_hit("vk_ok_"); }
int vk_own_cpu_read(u64 cpu, u64 size, u32 presents) { stub_hit("vk_own_cpu_read"); }
void vk_own_cpu_touched(u64 cpu, u64 size) { stub_hit("vk_own_cpu_touched"); }
void vk_own_publish(u64 cpu, u64 size, VkOwnRes* r, u64 off) { stub_hit("vk_own_publish"); }
VkOwnRes* vk_own_res_new(u64 size, u32 by) { stub_hit("vk_own_res_new"); }
void vk_own_res_ref(VkOwnRes* r) { stub_hit("vk_own_res_ref"); }
void vk_own_res_unref(VkOwnRes* r) { stub_hit("vk_own_res_unref"); }
int vk_own_watch(u64 cpu, u64 size) { stub_hit("vk_own_watch"); }
VkFormat vk_raw_format(u32 bpp) { stub_hit("vk_raw_format"); }
u8* vk_readback_buffer(u64 size, VkBuffer* buf) { stub_hit("vk_readback_buffer"); }
void vk_readback_ready(void) { stub_hit("vk_readback_ready"); }
void vk_render_end_at(const char* file, int line) { stub_hit("vk_render_end_at"); }
void vk_result_begin(void) { stub_hit("vk_result_begin"); }
void vk_result_end(void) { stub_hit("vk_result_end"); }
VkFmt vk_rt_format(u32 fmt) { stub_hit("vk_rt_format"); }
VkFormat vk_srgb_as(VkFormat f, int srgb) { stub_hit("vk_srgb_as"); }
u8* vk_stage(u64 size, u64 align, VkBuffer* buf, u64* off) { stub_hit("vk_stage"); }
u64 vk_stage_align(void) { stub_hit("vk_stage_align"); }
int vk_stage_device_at(const char* file, int line, u64 size, u64 align, VkBuffer* buf, u64* off) {
    stub_hit("vk_stage_device_at");
}
u8* vk_stage_guest_input(u64 gva, u64 source_size, u64 bytes, VkBuffer* buf, u64* off) {
    stub_hit("vk_stage_guest_input");
}
int vk_tc_blit(VkCommandBuffer cb, u32 bpp, u32 fmt, u32 nb, u32 rows, VkBuffer in, u64 in_size, u64 src_at,
               u32 in_pitch, u64 col_at, u64 row_at, VkBuffer out, u64 out_at, u32 out_pitch) {
    stub_hit("vk_tc_blit");
}
int vk_tex_can_coarsen(const Surf* t) { stub_hit("vk_tex_can_coarsen"); }
u64 vk_tex_coarser(Surf* t) { stub_hit("vk_tex_coarser"); }
u64 vk_tex_demote(Surf* t) { stub_hit("vk_tex_demote"); }
void vk_tex_evicted(const Surf* t) { stub_hit("vk_tex_evicted"); }
int vk_tex_finer(Surf* t) { stub_hit("vk_tex_finer"); }
void vk_tex_note_writeback(Surf* s) { stub_hit("vk_tex_note_writeback"); }
int vk_tex_promote(Surf* t) { stub_hit("vk_tex_promote"); }
int vk_uma_held(u64 cpu, u64 len) { stub_hit("vk_uma_held"); }
void vk_unlock(void) { stub_hit("vk_unlock"); }
void vk_view_formats(VkImageCreateInfo* ii, VkViewFormats* vf, VkFormat raw) { stub_hit("vk_view_formats"); }
void vk_write_changed(u64 addr, const u8* res, const u8* org, u32 size) { stub_hit("vk_write_changed"); }
void vk_written_runs(const u8* host, u64 len, u32 since, void (*fn)(void*, u64, u64), void* user) {
    stub_hit("vk_written_runs");
}
int vk_written_since(const u8* host, u64 len, u32 since) { stub_hit("vk_written_since"); }
void vk_ww_cool(const u8* host, u64 len) { stub_hit("vk_ww_cool"); }
void vk_ww_landed(u64 cpu, u64 size, const u8* bytes) { stub_hit("vk_ww_landed"); }
u32 vk_ww_now(void) { stub_hit("vk_ww_now"); }
VkFmt vk_zt_format(u32 fmt) { stub_hit("vk_zt_format"); }
int mem_lock_held(void) { stub_hit("mem_lock_held"); }

/* the clock of the resource statistics (vk_resource_begin) */
uint64_t plat_ns(void) { return 0; }
/* the texture objects the game ended (gpu.c): none */
u32 gpu_textures_ended(u64* out, u32 cap) { return 0; }
/* the buffer cache and the ASTC and BC7 forms of textures: nothing held there */
u64 vk_buf_reclaim(u32 before, u64 want) { return 0; }
void vk_astc_auto_trimmed(u64 usage, u64 left, u64 budget, int force) {}
int vk_astc_bc7(void) { return 0; }
void vk_astc_forget(const Surf* t, u32 level) {}
u64 vk_astc_rgba8_bytes(void) { return 0; }
int vk_bc1_tight(const char* why) { return 0; }
void vk_tex_later_forget(Surf* s) {}
/* no draw recorded and not submitted holds an image */
int vk_gfx_image_holds(const Surf* s) { return 0; }
/* a target's image and views go when the frames are done, and the pages its pixels cover with it */
void vk_defer_image(VkImage img, DevMem a, VkImageView v1, VkImageView v2) {}
void pend_add_va(PendPages* t, u64 gva, u64 size, int delta) {}
void vk_devmem_reserve_use(int on) {}

/* what the trim reads: the heap's use and budget, and the presents before now (memory.c) */
static u64 g_use, g_budget, g_deferred;
void vk_mem_use(u64* usage, u64* budget) {
    *usage = g_use;
    *budget = g_budget;
}
/* the present `presents` ago, or 60 more where a time is asked too: a second is 60 presents here */
u32 vk_present_before(u64 ago_ns, u32 presents) { return g_vk_present - presents - (ago_ns ? 60 : 0); }
u64 vk_deferred_bytes(void) { return 0; }
u64 vk_deferred_moved(void) { return 0; }
void vk_defer_bytes(u64 bytes) { g_deferred += bytes; }
void logf_(int lvl, const char* fmt, ...) {}

/* a render target of size bytes at gva, last used now, written at wseq */
static Surf* target(u64 gva, u64 size, u32 wseq) {
    Surf* s = calloc(1, sizeof *s);
    CHECK(s);
    s->gva = gva;
    s->is_rt = 1;
    s->rt_size = size;
    s->bytes = size;
    s->wseq = wseq;
    s->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    row_take(s);
    lru_add(s, VK_LRU_RT);
    return s;
}
/* s drawn at its address after the targets written before its wseq, as vk_rt_drawn puts it in the drawn index */
static void drawn_over(Surf* s) {
    if (g_ndrawn == g_capdrawn) {
        g_capdrawn = g_capdrawn ? g_capdrawn * 2 : 8;
        g_drawn = realloc(g_drawn, (size_t)g_capdrawn * sizeof *g_drawn);
        CHECK(g_drawn);
    }
    s->draw_wseq = s->wseq;
    s->drawn_from = s->gva;
    if (s->rt_size > g_drawn_max) g_drawn_max = s->rt_size;
    const int at = drawn_lower(s->drawn_from);
    memmove(&g_drawn[at + 1], &g_drawn[at], (size_t)(g_ndrawn - at) * sizeof *g_drawn);
    g_drawn[at] = s;
    g_ndrawn++;
    s->in_drawn = 1;
}
static u32 targets_held(void) {
    u32 n, parked;
    u64 bytes;
    vk_surf_stats(VK_LRU_RT, &n, &bytes, &parked);
    return n;
}
/* A target last used `ago` presents ago that a later one drew all over, and a trim: 1 when the trim took it */
static int trim_takes(int pageable, int force, u32 ago) {
    g_vk_present += 1000; /* a new present for each trim: one for an allocation runs once a present */
    g_vk.has_pageable = pageable;
    Surf* old = target(0x40000000, 1 << 20, 1);
    old->last_used = g_vk_present - ago;
    Surf* over = target(0x40000000, 1 << 20, 2);
    drawn_over(over);
    CHECK(rt_superseded(old) && !rt_superseded(over) && targets_held() == 2);
    /* past the start of a trim for headroom, within half a head of the budget: the steps after the targets drawn
       over take only what went unused for a second and eight presents */
    g_use = g_budget - trim_head(g_budget) * 3 / 4;
    CHECK(g_use > vk_mem_trim_start(g_budget));
    g_deferred = 0;
    vk_mem_trim(force ? 1 << 20 : 0, force);
    const u32 held = targets_held();
    CHECK(held == 1 ? g_deferred == 1 << 20 : held == 2 && !g_deferred); /* the old one went, or nothing */
    while (g_rows.lists[VK_LRU_RT].head) surf_evict(g_rows.surf[g_rows.lists[VK_LRU_RT].head]);
    CHECK(targets_held() == 0 && !g_ndrawn);
    return held == 1;
}

int main(void) {
    g_budget = 1ull << 30;
    g_vk_present = 1000;
    /* for headroom where the driver pages: kept a second and eight presents (68 presents here) */
    CHECK(!trim_takes(1, 0, 5) && !trim_takes(1, 0, 20) && !trim_takes(1, 0, 67) && trim_takes(1, 0, 69));
    CHECK(trim_takes(1, 1, 5) && !trim_takes(1, 1, 1)); /* for an allocation: taken after two presents */
    /* where the driver does not page: taken after two presents */
    CHECK(trim_takes(0, 0, 5) && !trim_takes(0, 0, 1) && trim_takes(0, 1, 5));
    printf("vk_mem_trim: the trims kept and took the targets drawn over as they should\n");
    return 0;
}
