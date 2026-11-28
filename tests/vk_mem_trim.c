/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The trim of the renderer's caches (gpu/vk/surface.c vk_mem_trim) and the render targets that later ones drew all
   over (rt_superseded), and the textures that took targets' pixels where later targets drew all over
   (tex_superseded). Such a target holds no one's pixels, and a trim takes it before anything else. Where the
   driver moves memory by priority (g_vk.has_pageable), a trim for headroom leaves it for a second and eight presents,
   as the sweep every 30 presents does: targets have the highest priority, and the driver moves textures first. A trim
   for an allocation (force), or one where the driver does not move memory, takes it after two presents, and so does
   one while the heap itself is at its budget, though the caches count room in the free space inside the blocks. A
   target whose pixels exist only on the GPU and can't move into system memory stays for its age and goes under the
   pressure; one set aside reads its memory again where its bytes changed; targets in system memory go while the
   system heap is short (surface.c rt_give_way, rt_aside, vk_rt_sys_frame). A parked target's storage result lands
   nowhere (flush_land). The copies of a live target that no bind asked for in VK_RT_COPY_TRIM_PRESENTS go under the
   trims' pressure, and stay with none (rt_copies_reclaim). A trim for an allocation counts the heap's own use, where
   the caches count the free space inside its blocks as room. A target set aside or moved into system memory lets its
   copies go, but a save copy. No Vulkan loader, device or GPU is used:
   the rest of the renderer is stubs, and a call that the trim must not make aborts */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma GCC diagnostic ignored "-Wmissing-field-initializers" /* surface.c builds without -Wextra */
#include "nx/gpu/vk/surface.c"
ShCaps g_spirv_translate; /* the translator's caps: the pool map off (surface.c written_on) */

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

[[noreturn]] static void stub_hit(const char* name) {
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
/* the guest memory behind a target set aside (rt_aside), for the checks that ask: 1 none mapped, 2 the GPU's
   [GUEST_GPU, +GUEST_SIZE) mapped at guest address 0x1000 (g_mem), with no write tracking; 0 for the trim, which
   asks nothing of it */
#define GUEST_GPU  0x70000000ull
#define GUEST_SIZE 0x2000ull
static int g_guest;
u64 gmmu_run(u64 gva, u64 max, u64* cpu) {
    if (!g_guest) stub_hit("gmmu_run");
    if (g_guest != 2 || gva < GUEST_GPU || gva >= GUEST_GPU + GUEST_SIZE) return 0;
    *cpu = 0x1000 + (gva - GUEST_GPU);
    return max < GUEST_GPU + GUEST_SIZE - gva ? max : GUEST_GPU + GUEST_SIZE - gva;
}
u64 gmmu_translate(u64 gva) {
    if (!g_guest) stub_hit("gmmu_translate");
    return g_guest == 2 && gva >= GUEST_GPU && gva < GUEST_GPU + GUEST_SIZE ? 0x1000 + (gva - GUEST_GPU) : 0;
}
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz) { stub_hit("gpu_mem_dirty_since"); }
u64 gpu_mem_generation(void) { stub_hit("gpu_mem_generation"); }
void gpu_mem_written(u64 gva, u64 size) { stub_hit("gpu_mem_written"); }
/* the guest's live and ended texture objects, plus hooks for ended-buffer reclaim and texture promotion checks */
static struct {
    u64 added;
    u64 live[8];
    u32 nlive;
    u64 ended[8];
    u32 nended;
    u64 buf_ended_avail;
    u32 buf_ended_calls;
    int promote_ok;
    Surf* promoted[8];
    u32 npromoted;
} g_tex_objs;
int gpu_texture_live(u64 gva) {
    for (u32 i = 0; i < g_tex_objs.nlive; i++)
        if (g_tex_objs.live[i] == gva) return 1;
    return 0;
}
u64 gpu_textures_added(void) { return g_tex_objs.added; }
int own_covered(u64 addr, u64 size) { stub_hit("own_covered"); }
int own_hash(u64 addr, u64 size, u64* h) {
    if (!g_guest) stub_hit("own_hash");
    return 0; /* no page the GPU holds */
}
u32 own_lookup(u64 addr, u64 size, OwnRange* out, u32 max) { stub_hit("own_lookup"); }
int own_owed_take(u64* addr, u64* size) { stub_hit("own_owed_take"); }
int own_traps(void) { stub_hit("own_traps"); }
int pend_hit_va(const PendPages* t, u64 gva, u64 size) { stub_hit("pend_hit_va"); }
u8* plat_alias_mirror(void) { stub_hit("plat_alias_mirror"); }
void pm_lock(PMutex* m) { stub_hit("pm_lock"); }
void pm_unlock(PMutex* m) { stub_hit("pm_unlock"); }
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
u64 vk_hash(const void* p, u64 size) {
    if (!g_guest) stub_hit("vk_hash");
    u64 h = 1469598103934665603ull;
    for (u64 i = 0; i < size; i++) h = (h ^ ((const u8*)p)[i]) * 1099511628211ull;
    return h;
}
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
u64 vk_memory_blocks(void) { stub_hit("vk_memory_blocks"); }
u64 vk_bc7_cache_release(u32 before) { stub_hit("vk_bc7_cache_release"); }
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
int vk_render_begun(void) { stub_hit("vk_render_begun"); }
void vk_rt_copy_count(int why, int ended, u64 bytes, VkFormat from, VkFormat to) { stub_hit("vk_rt_copy_count"); }
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
u32 vk_mem_owner_prio(u32 owner) { stub_hit("vk_mem_owner_prio"); }
static u32 g_tex_evictions; /* textures the cache let go (tex_superseded_takes) */
void vk_tex_evicted(const Surf* t) { g_tex_evictions++; }
int vk_tex_image_own(Surf* s, VkImage* img, DevMem* mem, u64* bytes) {
    (void)s, (void)img, (void)mem, (void)bytes;
    return 0;
}
int vk_tex_finer(Surf* t) { stub_hit("vk_tex_finer"); }
void vk_tex_note_writeback(Surf* s) { stub_hit("vk_tex_note_writeback"); }
void vk_tex_sys_left(Surf* t) {
    if (t->is_rt || !t->demoted) return;
    t->demoted = 0;
    t->sys_bytes = 0;
    t->sys_reads = 0;
    t->sys_read_at = 0;
}
int vk_tex_promote(Surf* t) {
    if (!g_tex_objs.promote_ok) stub_hit("vk_tex_promote");
    if (g_tex_objs.npromoted < 8) g_tex_objs.promoted[g_tex_objs.npromoted++] = t;
    const u64 b = t->sys_bytes;
    t->demoted = 0;
    t->sys_bytes = 0;
    vk_surf_bytes(t, b);
    return 1;
}
int vk_uma_held(u64 cpu, u64 len) { stub_hit("vk_uma_held"); }
void vk_unlock(void) { stub_hit("vk_unlock"); }
void vk_view_formats(VkImageCreateInfo* ii, VkViewFormats* vf, VkFormat raw) { stub_hit("vk_view_formats"); }
void vk_write_changed(u64 addr, const u8* res, const u8* org, u32 size) { stub_hit("vk_write_changed"); }
void vk_written_runs(const u8* host, u64 len, u32 since, void (*fn)(void*, u64, u64), void* user) {
    stub_hit("vk_written_runs");
}
int vk_written_since(const u8* host, u64 len, u32 since) {
    if (!g_guest) stub_hit("vk_written_since");
    return -1; /* no write tracking */
}
void vk_ww_cool(const u8* host, u64 len) { stub_hit("vk_ww_cool"); }
void vk_ww_landed(u64 cpu, u64 size, const u8* bytes) { stub_hit("vk_ww_landed"); }
u32 vk_ww_now(void) { stub_hit("vk_ww_now"); }
VkFmt vk_zt_format(u32 fmt) { stub_hit("vk_zt_format"); }
int mem_lock_held(void) { stub_hit("mem_lock_held"); }

/* the clock of the resource statistics (vk_resource_begin) */
uint64_t plat_ns(void) { return 0; }
/* the texture objects the game ended (gpu.c) */
u32 gpu_textures_ended(u64* out, u32 cap) {
    u32 n = g_tex_objs.nended < cap ? g_tex_objs.nended : cap;
    for (u32 i = 0; i < n; i++) out[i] = g_tex_objs.ended[i];
    g_tex_objs.nended -= n;
    return n;
}
/* the buffer cache and the ASTC and BC7 forms of textures: nothing held there unless a test seeds ended buffers */
u64 vk_buf_reclaim(u32 before, u64 want) { return 0; }
u64 vk_buf_reclaim_ended(u32 before, u64 want) {
    g_tex_objs.buf_ended_calls++;
    u64 got = g_tex_objs.buf_ended_avail < want ? g_tex_objs.buf_ended_avail : want;
    g_tex_objs.buf_ended_avail -= got;
    return got;
}
void vk_astc_auto_trimmed(u64 usage, u64 left, u64 budget, int force) {}
void vk_stamp_runtime_begin(void) {}
void vk_stamp_runtime_end(void) {}
int vk_astc_bc7(void) { return 0; }
void vk_astc_forget(const Surf* t, u32 level) {}
u64 vk_astc_rgba8_bytes(void) { return 0; }
int vk_bc1_tight(const char* why) { return 0; }
void vk_tex_later_forget(Surf* s) {}
/* no draw recorded and not submitted holds an image */
int vk_gfx_image_holds(const Surf* s) { return 0; }
int vk_gfx_image_pending_besides(u64 gva, u64 size, const Surf* self) { return 0; }
void vk_gfx_image_mem_need(u64 gva, u64 size) { stub_hit("vk_gfx_image_mem_need"); }
/* a target's image and views go when the frames are done, and the pages its pixels cover with it */
void vk_defer_image(VkImage img, DevMem a, VkImageView v1, VkImageView v2) {}
void pend_add_va(PendPages* t, u64 gva, u64 size, int delta) {}
void vk_devmem_reserve_use(int on) {}

/* the system heap's use and budget as the driver tells them (devmem.c), heap 0 here */
static u64 g_sys_use, g_sys_budget;
void vk_devmem_budgets(DevMemBudget* b) {
    memset(b, 0, sizeof *b * VK_MAX_MEMORY_HEAPS);
    b[0].usage = g_sys_use;
    b[0].budget = g_sys_budget;
}
/* what the trim reads: the heap's use and budget, and the presents before now (memory.c) */
static u64 g_use, g_budget, g_deferred, g_heap_use;
void vk_mem_use(u64* usage, u64* budget) {
    *usage = g_use;
    *budget = g_budget;
}
/* the heap's use with the free space inside the blocks (memory.c) */
u64 vk_mem_heap_use(void) { return g_heap_use; }
/* the present `presents` ago, or 60 more where a time is asked too: a second is 60 presents here */
u32 vk_present_before(u64 ago_ns, u32 presents) { return g_vk_present - presents - (ago_ns ? 60 : 0); }
/* what evictions before now are releasing, which the frames in flight still hold (cmd.c) */
static u64 g_releasing;
u64 vk_deferred_bytes(void) { return g_releasing; }
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
        Surf** n = realloc(g_drawn, (size_t)g_capdrawn * sizeof *n);
        CHECK(n);
        g_drawn = n;
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

/* A target last used `ago` presents ago that a later one drew all over, and a trim for headroom where the driver
   pages, with the caches' use (the free space inside the blocks counted as room) under the trim's start and the
   heap's own use at its budget (heap_full) or under the start: 1 when the trim took it */
static int heap_full_takes(int heap_full, u32 ago) {
    g_vk_present += 1000;
    g_vk.has_pageable = 1;
    Surf* old = target(0x40000000, 1 << 20, 1);
    old->last_used = g_vk_present - ago;
    Surf* over = target(0x40000000, 1 << 20, 2);
    drawn_over(over);
    g_use = vk_mem_trim_start(g_budget) - trim_head(g_budget);
    g_heap_use = heap_full ? g_budget : g_use;
    g_deferred = 0;
    vk_mem_trim(0, 0);
    const u32 held = targets_held();
    CHECK(held == 1 ? g_deferred == 1 << 20 : held == 2 && !g_deferred);
    while (g_rows.lists[VK_LRU_RT].head) surf_evict(g_rows.surf[g_rows.lists[VK_LRU_RT].head]);
    CHECK(targets_held() == 0 && !g_ndrawn);
    g_heap_use = 0;
    return held == 1;
}

/* A live target that the game samples whole again holds the copies rt_copy made of its corners at sizes it went
   through, which rt_copy lets go only when a bind asks it for another copy. A trim under pressure lets go of those no
   bind asked for in VK_RT_COPY_TRIM_PRESENTS presents, with their bytes: a trim for headroom (how 1), and a trim while
   the heap is at its budget though the caches count room in the blocks (2). The copy a bind asked for 100 presents
   ago, a feedback draw's save copy (predraw) and the target stay. With no pressure (0) none goes: a game at a dynamic
   resolution asks again for the copies of the sizes it goes back to. 1 when it went so */
static int copies_go(int how) {
    static const u32 ago[] = {VK_RT_COPY_TRIM_PRESENTS + 100, VK_RT_COPY_TRIM_PRESENTS + 200,
                              VK_RT_COPY_TRIM_PRESENTS + 400, 100, VK_RT_COPY_TRIM_PRESENTS + 100};
    g_vk_present += 1000;
    g_vk.has_pageable = 0;
    Surf* rt = target(0x60000000, 8 << 20, 1);
    rt->last_used = g_vk_present;
    Surf* kept[2] = {NULL, NULL};
    for (int i = 0; i < 5; i++) {
        Surf* c = calloc(1, sizeof *c);
        CHECK(c);
        c->is_rt = 1;
        c->gva = rt->gva;
        c->bytes = (u64)(i + 1) << 20;
        c->last_used = g_vk_present - ago[i];
        c->predraw = i == 4;
        c->next = rt->crop;
        rt->crop = c;
        if (i >= 3) kept[i - 3] = c;
    }
    VkRtKinds was, now;
    vk_rt_kinds(&was);
    g_deferred = 0;
    if (how == 0) {
        g_use = vk_mem_trim_start(g_budget) - trim_head(g_budget);
        g_heap_use = g_use;
        vk_mem_trim(0, 0);
        g_heap_use = 0;
    }
    if (how == 1) {
        g_use = g_budget - trim_head(g_budget) * 3 / 4;
        vk_mem_trim(0, 0);
    }
    if (how == 2) {
        g_use = vk_mem_trim_start(g_budget) - trim_head(g_budget);
        g_heap_use = g_budget;
        vk_mem_trim(0, 0);
        g_heap_use = 0;
    }
    u32 left = 0;
    for (const Surf* x = rt->crop; x; x = x->next) left++;
    vk_rt_kinds(&now); /* the report's counts: the copies held, and those let go */
    const u64 gone = now.copies_gone - was.copies_gone, gone_bytes = now.copies_gone_bytes - was.copies_gone_bytes;
    const int went = how ? left == 2 && rt->crop == kept[1] && kept[1]->next == kept[0] && g_deferred == 6 << 20 &&
                               now.copies == 2 && now.copy_bytes == 9 << 20 && gone == 3 && gone_bytes == 6 << 20
                         : left == 5 && !g_deferred && now.copies == 5 && !gone;
    if (!went || targets_held() != 1)
        fprintf(stderr,
                "copies_go(%d): %u copies left, %llu bytes deferred, %u targets; the report: %u held, %llu let go\n",
                how, left, (unsigned long long)g_deferred, targets_held(), now.copies, (unsigned long long)gone);
    const int target_stays = targets_held() == 1;
    surf_evict(rt);
    CHECK(targets_held() == 0);
    return went && target_stays;
}

/* A texture of 1 MB at 0x50000000 that took pixels from targets when the surfaces' write count was 10, last used
   five presents ago, and targets drawn over its memory: (over_from, over_size, over_wseq) each. 1 when it is no one's
   (tex_superseded) and the sweep (vk_tex_reclaim_superseded) let it go, 0 when it stayed, -1 when the two disagree */
typedef struct {
    u64 from, size;
    u32 wseq;
} Over;
static int tex_superseded_takes(int took, const Over* over, u32 n) {
    g_vk_present += 1000;
    Surf* t = calloc(1, sizeof *t);
    CHECK(t);
    t->gva = 0x50000000;
    t->size = t->bytes = 1 << 20;
    t->took_drawn = took;
    g_wseq = 10;
    if (took) vk_tex_took(t);
    row_take(t);
    lru_add(t, VK_LRU_TEX);
    t->last_used = g_vk_present - 5;
    for (u32 i = 0; i < n; i++) {
        Surf* o = target(over[i].from, over[i].size, over[i].wseq);
        drawn_over(o);
    }
    const int gone = tex_superseded(t), lost = lossless(t);
    const u32 evictions = g_tex_evictions;
    g_deferred = 0;
    /* used two presents ago or more: five here */
    const u64 freed = vk_tex_reclaim_superseded(vk_present_before(0, 2), ~0ull);
    const int went = g_tex_evictions == evictions + 1 && freed == 1 << 20 && g_deferred == freed;
    if (!went) surf_evict(t);
    while (g_rows.lists[VK_LRU_RT].head) surf_evict(g_rows.surf[g_rows.lists[VK_LRU_RT].head]);
    CHECK(!g_ndrawn && !g_rows.lists[VK_LRU_TEX].head);
    if (gone != went || (took && lost != gone)) return -1;
    return went;
}

/* A target whose pixels exist only on the GPU, where its image can't move into system memory (in pieces here: no
   memory of its own) or where the GPU shares the system's memory (VK_MEM_UMA): the age's walk (VK_RECLAIM_IDLE) keeps
   it and sets it aside, the pressure's (VK_RECLAIM_ANY) takes it, counted as lost. 1 when that held */
static int aside_or_lost(int model) {
    g_vk.mem_model = model;
    g_vk_present += 1000;
    Surf* s = target(0x50000000, 1 << 20, 1);
    s->last_used = g_vk_present - 100;
    CHECK(!lossless(s) && !keeps_drawn(s));
    const u64 kept = g_rt_sys.kept, lost = g_rt_sys.lost;
    g_guest = 1;
    vk_surf_reclaim(VK_LRU_RT, g_vk_present, ~0ull, VK_RECLAIM_IDLE);
    g_guest = 0;
    const int stayed = targets_held() == 1 && s->aside && g_rt_sys.kept == kept + 1 && g_rt_sys.lost == lost;
    g_deferred = 0;
    vk_surf_reclaim(VK_LRU_RT, g_vk_present, 1, VK_RECLAIM_ANY);
    const int went =
        targets_held() == 0 && g_deferred == 1 << 20 && g_rt_sys.lost == lost + 1 && g_rt_sys.lost_bytes >= 1 << 20;
    g_vk.mem_model = VK_MEM_SMALL_BAR;
    return stayed && went;
}
/* Without write tracking a target set aside keeps its memory's hash (rt_aside), and its next use loads memory again
   once a byte of it changed (rt_aside_check). 1 when no change loaded nothing and a change loaded it */
static int aside_hash(void) {
    static u8 mem[0x1000 + GUEST_SIZE];
    for (u32 i = 0; i < sizeof mem; i++) mem[i] = (u8)(i * 7);
    g_mem = mem;
    g_guest = 2;
    Surf* s = target(GUEST_GPU, GUEST_SIZE, 1);
    const u64 reloads = g_rt_sys.reloads;
    rt_aside(s);
    const int hashed = s->aside && !s->aside_wseen;
    rt_aside_check(s);
    const int unchanged = !s->aside && g_rt_sys.reloads == reloads;
    rt_aside(s);
    mem[0x1000 + GUEST_SIZE - 1] ^= 1; /* its last byte */
    rt_aside_check(s);
    const int changed = !s->aside && g_rt_sys.reloads == reloads + 1;
    g_guest = 0;
    g_mem = NULL;
    surf_evict(s);
    return hashed && unchanged && changed;
}
/* A parked target (rt_create_ parks the one at an address that another size takes, its address with bit 63 set) whose
   storage image's result lands (flush_land, from image_out.c's landing): memory there is no longer its own, so nothing
   is read, written or stamped (each of those calls aborts here) and the result is settled. A write stamped at the
   parked address read as one over every page, and loaded every target again. 1 when that held */
static int parked_lands_nothing(void) {
    static u8 pixels[GUEST_SIZE], original[GUEST_SIZE];
    Surf* s = target(GUEST_GPU | 1ull << 63, GUEST_SIZE, 1);
    g_parked++; /* as rt_create_ counts it */
    s->wgen = 7;
    s->mem_wgen = 3;
    u8* keep = NULL;
    u64 keep_size = 0;
    flush_land(s, pixels, original, s->wgen, &keep, &keep_size);
    const int settled = s->mem_wgen == 7 && !keep && !keep_size;
    surf_evict(s);
    return settled && targets_held() == 0;
}
/* Targets in system memory (rt_demote) while the system heap they went to is past its budget less a head: every 30
   presents the least recently used go until it is within, their pixels with them; one the last two presents used
   stays, however far past the heap is. 1 when the oldest alone went for an excess within its size, and the other old
   one, not the one used lately, for an excess past all three */
static int sys_heap_short(void) {
    g_vk_present = (g_vk_present / 30 + 1) * 30 + 30; /* a present the check runs at */
    Surf* s[3];
    for (int i = 0; i < 3; i++) {
        s[i] = target(0x60000000 + (u64)i * 0x1000000, 1 << 20, 1);
        s[i]->last_used = g_vk_present - (i < 2 ? 100 - (u32)i : 0); /* the first the oldest, the last used now */
        s[i]->demoted = 1;
        s[i]->sys_bytes = 4u << 20;
        vk_surf_bytes(s[i], 0);
        g_rt_sys.n++;
        g_rt_sys.bytes += 4u << 20;
    }
    g_rt_sys.heap = 1;
    g_sys_budget = 1ull << 30;
    g_sys_use = g_sys_budget - trim_head(g_sys_budget) + (2u << 20); /* 2 MiB past: the oldest's 4 MiB covers it */
    g_use = g_budget;                                                /* no room in the GPU's memory: none goes back */
    const u64 lost = g_rt_sys.lost;
    vk_rt_sys_frame();
    const int oldest = targets_held() == 2 && g_rt_sys.lost == lost + 1 && g_rt_sys.n == 2 &&
                       g_rt_sys.bytes == 8u << 20 && g_rows.surf[g_rows.lists[VK_LRU_RT].head] == s[1];
    g_vk_present += 30;
    s[2]->last_used = g_vk_present;
    g_sys_use = g_sys_budget - trim_head(g_sys_budget) + (64u << 20); /* past them all */
    vk_rt_sys_frame();
    const int hot_stays = targets_held() == 1 && g_rt_sys.lost == lost + 2 && g_rt_sys.n == 1 &&
                          g_rows.surf[g_rows.lists[VK_LRU_RT].head] == s[2] && s[2]->demoted;
    while (g_rows.lists[VK_LRU_RT].head) surf_evict(g_rows.surf[g_rows.lists[VK_LRU_RT].head]);
    return oldest && hot_stays && g_rt_sys.n == 0 && g_rt_sys.bytes == 0;
}

/* A trim for an allocation that found no memory while the free space inside the heap's blocks is in pieces: the use
   the caches count (vk_mem_use, without those pieces) is under every step's mark, while the heap's own use is at its
   budget. A superseded target satisfies `need`, so only counting the heap's own use reaches the lossy step and takes
   the target with pixels memory lacks. 1 when both targets went */
static int forced_in_pieces(void) {
    g_vk_present += 1000;
    g_vk.has_pageable = 0;
    Surf* s = target(0x70000000, 16 << 20, 1);
    s->last_used = g_vk_present - 1000;
    Surf* old = target(0x70800000, 16 << 20, 2);
    old->last_used = g_vk_present - 10;
    Surf* over = target(0x70800000, 16 << 20, 3);
    drawn_over(over);
    CHECK(rt_superseded(old) && !lossless(s));
    g_use = g_budget - 3 * trim_head(g_budget); /* under every step's mark */
    g_heap_use = g_budget;
    g_releasing = 0;
    g_deferred = 0;
    const u64 lost = g_rt_sys.lost;
    vk_mem_trim(8 << 20, 1);
    const int went = targets_held() == 1 && g_deferred == 32 << 20 && g_rt_sys.lost == lost + 1;
    if (!went)
        fprintf(stderr, "forced_in_pieces: %u targets held, %llu bytes deferred\n", targets_held(),
                (unsigned long long)g_deferred);
    while (g_rows.lists[VK_LRU_RT].head) surf_evict(g_rows.surf[g_rows.lists[VK_LRU_RT].head]);
    g_heap_use = 0;
    return went;
}

/* A trim for an allocation while both vk_mem_use and the raw heap use are under every step's mark, and evictions
   before it are releasing more than the allocation: `need` stays the full allocation rather than subtracting what is
   releasing, so the lossy step still runs and takes the target. 1 when the target went */
static int forced_need_ignores_deferred(void) {
    g_vk_present += 1000;
    g_vk.has_pageable = 0;
    Surf* s = target(0x70000000, 16 << 20, 1);
    s->last_used = g_vk_present - 1000;
    CHECK(!lossless(s));
    const u64 low = g_budget - 3 * trim_head(g_budget); /* raw heap + releasing + extra stays under every mark */
    g_use = low;
    g_heap_use = low;
    g_releasing = 64 << 20; /* more than the allocation */
    g_deferred = 0;
    const u64 lost = g_rt_sys.lost;
    vk_mem_trim(8 << 20, 1);
    const int went = targets_held() == 0 && g_deferred == 16 << 20 && g_rt_sys.lost == lost + 1;
    if (!went)
        fprintf(stderr, "forced_need_ignores_deferred: %u targets held, %llu bytes deferred\n", targets_held(),
                (unsigned long long)g_deferred);
    while (g_rows.lists[VK_LRU_RT].head) surf_evict(g_rows.surf[g_rows.lists[VK_LRU_RT].head]);
    g_heap_use = 0;
    g_releasing = 0;
    return went;
}

/* A target with pixels memory lacks that the age's walk sets aside (VK_RECLAIM_IDLE; its image can't move here) lets
   its copies go, a corner's and a feedback draw's snapshot: a bind makes them again from the target. Its save copy
   (predraw) stays, as the trim's sweep leaves it (rt_copies_reclaim). The report counts the copies held by what they
   were made for. Copies of a target set aside or moved into system memory stayed in the GPU's memory: 1.9 GB of them
   in a racing game whose heap was at its budget. 1 when the two went and the save copy stayed */
static int copies_go_aside(void) {
    g_vk.mem_model = VK_MEM_SMALL_BAR;
    g_vk_present += 1000;
    Surf* s = target(0x71000000, 4 << 20, 1);
    s->last_used = g_vk_present - 100;
    static const int why[3] = {VK_RT_COPY_CROP, VK_RT_COPY_FEEDBACK, VK_RT_COPY_FEEDBACK};
    Surf* save = NULL;
    for (int i = 0; i < 3; i++) {
        Surf* c = calloc(1, sizeof *c);
        CHECK(c);
        c->is_rt = 1;
        c->gva = s->gva;
        c->bytes = (u64)(i + 1) << 20;
        c->last_used = g_vk_present - 100;
        c->copy_why = (u8)why[i];
        c->predraw = i == 2;
        c->next = s->crop;
        s->crop = c;
        if (i == 2) save = c;
    }
    VkRtKinds was, now;
    vk_rt_kinds(&was);
    const int counted = was.copies == 3 && was.copies_by[VK_RT_COPY_CROP] == 1 &&
                        was.copy_bytes_by[VK_RT_COPY_CROP] == 1 << 20 && was.copies_by[VK_RT_COPY_FEEDBACK] == 2 &&
                        was.copy_bytes_by[VK_RT_COPY_FEEDBACK] == 5 << 20;
    g_deferred = 0;
    g_guest = 1;
    vk_surf_reclaim(VK_LRU_RT, g_vk_present, ~0ull, VK_RECLAIM_IDLE);
    g_guest = 0;
    vk_rt_kinds(&now);
    const int went = targets_held() == 1 && s->aside && s->crop == save && !save->next && g_deferred == 3 << 20 &&
                     now.copies == 1 && now.copies_by[VK_RT_COPY_FEEDBACK] == 1 && !now.copies_by[VK_RT_COPY_CROP] &&
                     now.copies_gone - was.copies_gone == 2;
    if (!counted || !went)
        fprintf(stderr,
                "copies_go_aside: %u copies held before (%u corners), %u after, %llu bytes deferred, set aside %d\n",
                was.copies, was.copies_by[VK_RT_COPY_CROP], now.copies, (unsigned long long)g_deferred, s->aside);
    surf_evict(s);
    CHECK(targets_held() == 0);
    return counted && went;
}

/* A trim for headroom while the free space inside the heap's blocks is in pieces: vk_mem_use is under the trim's
   start, while vk_mem_heap_use is at its budget. Freeing live entries inside blocks only widens the holes without
   lowering vk_mem_heap_use until whole blocks empty, so a non-forced trim sweeps only dead entries (superseded
   surfaces, stale copies, ended textures) once a present and leaves live non-superseded targets alone, even when
   called again on the same or a later present with vk_mem_heap_use still at the budget. 1 when the superseded target
   went, the live target stayed, and a second call freed nothing */
static int headroom_in_pieces(void) {
    g_vk_present += 1000;
    g_vk.has_pageable = 0;
    Surf* live = target(0x72000000, 16 << 20, 1);
    live->last_used = g_vk_present - 1000;
    Surf* old = target(0x72800000, 16 << 20, 2);
    old->last_used = g_vk_present - 10;
    Surf* over = target(0x72800000, 16 << 20, 3);
    drawn_over(over);
    CHECK(rt_superseded(old) && !lossless(live));
    g_use = g_budget - 3 * trim_head(g_budget); /* under the trim's start and every step's mark */
    g_heap_use = g_budget;
    g_deferred = 0;
    const u64 lost = g_rt_sys.lost;
    vk_mem_trim(0, 0);
    const int first = targets_held() == 2 && g_deferred == 16 << 20 && g_rt_sys.lost == lost;
    /* a second call in the same present, and another on a later present with the heap still at the budget, free no
       live target */
    g_deferred = 0;
    vk_mem_trim(0, 0);
    g_vk_present += 10;
    vk_mem_trim(0, 0);
    const int quiet = targets_held() == 2 && g_deferred == 0 && g_rt_sys.lost == lost;
    if (!first || !quiet)
        fprintf(stderr, "headroom_in_pieces: first=%d quiet=%d, %u targets held, %llu bytes deferred\n", first, quiet,
                targets_held(), (unsigned long long)g_deferred);
    while (g_rows.lists[VK_LRU_RT].head) surf_evict(g_rows.surf[g_rows.lists[VK_LRU_RT].head]);
    g_heap_use = 0;
    return first && quiet;
}

/* A trim for an allocation while deferred frees hold enough bytes to pull vk_mem_heap_use under the lossy steps'
   marks: those frees return to the blocks as pieces that need not hold the failed request's run. Even after a
   superseded target satisfies `need`, the forced trim still counts the deferred bytes in the heap's use so the
   lossy steps run. 1 when the lossy target went */
static int forced_counts_deferred_heap(void) {
    g_vk_present += 1000;
    g_vk.has_pageable = 0;
    const u64 head = trim_head(g_budget);
    Surf* s = target(0x74000000, 16 << 20, 1);
    s->last_used = g_vk_present - 1000;
    Surf* old = target(0x73000000, 16 << 20, 2);
    old->last_used = g_vk_present - 10;
    Surf* over = target(0x73000000, 16 << 20, 3);
    drawn_over(over);
    CHECK(rt_superseded(old) && !lossless(s));
    g_use = g_budget - 3 * head;
    g_heap_use = g_budget - head - (16 << 20); /* below steps[2].mark until deferred bytes are added back */
    g_releasing = head;
    g_deferred = 0;
    const u64 lost = g_rt_sys.lost;
    vk_mem_trim(8 << 20, 1);
    const int went = targets_held() == 1 && g_deferred == 32 << 20 && g_rt_sys.lost == lost + 1;
    if (!went)
        fprintf(stderr, "forced_counts_deferred_heap: %u targets held, %llu bytes deferred\n", targets_held(),
                (unsigned long long)g_deferred);
    while (g_rows.lists[VK_LRU_RT].head) surf_evict(g_rows.surf[g_rows.lists[VK_LRU_RT].head]);
    g_heap_use = 0;
    g_releasing = 0;
    return went;
}

/* A trim for an allocation that found no memory (force) lets go of target copies unused for VK_RT_COPY_PRESENTS
   presents before the lossy steps take targets, while keeping recent copies and save copies (predraw). 1 when the
   copy unused for 100 presents went and the 50-present copy and save copy stayed */
static int forced_copies_go(void) {
    static const u32 ago[3] = {VK_RT_COPY_PRESENTS + 40, VK_RT_COPY_PRESENTS - 10, VK_RT_COPY_PRESENTS + 40};
    static const u64 bytes[3] = {4 << 20, 2 << 20, 5 << 20};
    g_vk_present += 1000;
    g_vk.has_pageable = 0;
    Surf* rt = target(0x75000000, 8 << 20, 1);
    rt->last_used = g_vk_present;
    Surf* kept[2] = {NULL, NULL};
    for (int i = 0; i < 3; i++) {
        Surf* c = calloc(1, sizeof *c);
        CHECK(c);
        c->is_rt = 1;
        c->gva = rt->gva;
        c->bytes = bytes[i];
        c->last_used = g_vk_present - ago[i];
        c->predraw = i == 2;
        c->next = rt->crop;
        rt->crop = c;
        if (i >= 1) kept[i - 1] = c;
    }
    g_use = g_budget - 3 * trim_head(g_budget);
    g_heap_use = g_budget;
    g_deferred = 0;
    vk_mem_trim(8 << 20, 1);
    u32 left = 0;
    for (const Surf* x = rt->crop; x; x = x->next) left++;
    const int went =
        left == 2 && rt->crop == kept[1] && kept[1]->next == kept[0] && g_deferred == 4 << 20 && targets_held() == 1;
    if (!went)
        fprintf(stderr, "forced_copies_go: %u copies left, %llu bytes deferred, %u targets\n", left,
                (unsigned long long)g_deferred, targets_held());
    surf_evict(rt);
    CHECK(targets_held() == 0);
    g_heap_use = 0;
    return went;
}

/* Demoted render targets stay while the system heap has headroom, however long they went unused; under system-heap
   pressure, those with no live texture object at their address (parked bit 63 stripped) go before older ones whose
   object is still live, and live-object targets go oldest first only for what remains. 1 when all three held */
static int sys_dead_before_live(void) {
    g_vk_present = (g_vk_present / 30 + 1) * 30 + 10020;
    Surf* s[4];
    static const u64 gvas[4] = {0x76400000ull, 0x76800000ull, 0x76c00000ull | (1ull << 63),
                                0x77000000ull | (1ull << 63)};
    static const u32 ages[4] = {10000u, 100u, 50u, 40u};
    for (int i = 0; i < 4; i++) {
        s[i] = target(gvas[i], 4 << 20, 1 + (u32)i);
        s[i]->last_used = g_vk_present - ages[i];
        s[i]->demoted = 1;
        s[i]->sys_bytes = 4u << 20;
        vk_surf_bytes(s[i], 0);
        g_rt_sys.n++;
        g_rt_sys.bytes += 4u << 20;
    }
    /* s[0] (oldest) and s[2] (parked) have live texture objects; s[1] and s[3] (parked) have none */
    g_tex_objs.added = 2;
    g_tex_objs.live[0] = 0x76400000ull;
    g_tex_objs.live[1] = 0x76c00000ull;
    g_tex_objs.nlive = 2;
    g_rt_sys.heap = 1;
    g_sys_budget = 16ull << 30;
    g_sys_use =
        12ull << 30; /* headroom under budget - budget/8 (14 GiB): nothing leaves despite 10,000 presents idle */
    g_use = g_budget;
    const u64 lost = g_rt_sys.lost;
    vk_rt_sys_frame();
    const int kept = targets_held() == 4 && g_rt_sys.lost == lost && g_rt_sys.n == 4 && g_rt_sys.bytes == 16u << 20;
    /* 2 MiB over sys_high: younger dead s[1] goes before older live s[0] */
    g_sys_use = (14ull << 30) + (2u << 20);
    vk_rt_sys_frame();
    const int dead_first = targets_held() == 3 && g_rt_sys.lost == lost + 1 && g_rt_sys.n == 3 &&
                           g_rows.surf[g_rows.lists[VK_LRU_RT].head] == s[0] && s[0]->demoted && s[2]->demoted;
    /* 6 MiB over sys_high after wait: parked dead s[3] (4 MiB) goes first, then oldest live s[0] (4 MiB), leaving
       parked live s[2] */
    g_vk_present += RT_SYS_PRESS_WAIT;
    g_sys_use = (14ull << 30) + (6u << 20);
    vk_rt_sys_frame();
    const int fallback = targets_held() == 1 && g_rt_sys.lost == lost + 3 && g_rt_sys.n == 1 &&
                         g_rows.surf[g_rows.lists[VK_LRU_RT].head] == s[2] && s[2]->demoted;
    if (!kept || !dead_first || !fallback)
        fprintf(stderr, "sys_dead_before_live: kept=%d dead_first=%d fallback=%d, %u held, %llu lost\n", kept,
                dead_first, fallback, targets_held(), (unsigned long long)(g_rt_sys.lost - lost));
    while (g_rows.lists[VK_LRU_RT].head) surf_evict(g_rows.surf[g_rows.lists[VK_LRU_RT].head]);
    g_tex_objs.added = 0;
    g_tex_objs.nlive = 0;
    return kept && dead_first && fallback && g_rt_sys.n == 0 && g_rt_sys.bytes == 0;
}

/* On a large system heap, headroom scales with the budget (an eighth, not capped at 256 MiB) and hard pressure runs on
   any present (not only every 30), then waits RT_SYS_PRESS_WAIT presents for deferred frees unless usage climbs. 1 when
   the oldest went on a non-multiple-of-30 present, the next stayed while usage was flat until the wait elapsed, and the
   third went without waiting once usage climbed */
static int sys_pressure_scales_and_paces(void) {
    g_vk_present = (g_vk_present / 30 + 1) * 30 + 7; /* not a multiple of 30 */
    Surf* s[3];
    for (int i = 0; i < 3; i++) {
        s[i] = target(0x77000000 + (u64)i * 0x400000, 4 << 20, 1 + (u32)i);
        s[i]->last_used = g_vk_present - (100u - (u32)i);
        s[i]->demoted = 1;
        s[i]->sys_bytes = 4u << 20;
        vk_surf_bytes(s[i], 0);
        g_rt_sys.n++;
        g_rt_sys.bytes += 4u << 20;
    }
    g_rt_sys.heap = 1;
    g_sys_budget = 16ull << 30;
    g_sys_use = (14ull << 30) + (2u << 20); /* past budget - budget/8, ~2 GiB under budget - 256 MiB */
    g_use = g_budget;
    const u64 lost = g_rt_sys.lost;
    vk_rt_sys_frame();
    const int immediate = targets_held() == 2 && g_rt_sys.lost == lost + 1 && g_rt_sys.n == 2;
    g_vk_present++;
    vk_rt_sys_frame();
    const int paced = targets_held() == 2 && g_rt_sys.lost == lost + 1;
    g_vk_present += RT_SYS_PRESS_WAIT - 1u;
    vk_rt_sys_frame();
    const int after_wait = targets_held() == 1 && g_rt_sys.lost == lost + 2 && g_rt_sys.n == 1;
    g_vk_present++;
    g_sys_use += 64ull << 20; /* usage climbed: evict without waiting */
    vk_rt_sys_frame();
    const int climbed = targets_held() == 0 && g_rt_sys.lost == lost + 3 && g_rt_sys.n == 0;
    if (!immediate || !paced || !after_wait || !climbed)
        fprintf(stderr, "sys_pressure_scales_and_paces: immediate=%d paced=%d after_wait=%d climbed=%d, %u held\n",
                immediate, paced, after_wait, climbed, targets_held());
    while (g_rows.lists[VK_LRU_RT].head) surf_evict(g_rows.surf[g_rows.lists[VK_LRU_RT].head]);
    return immediate && paced && after_wait && climbed && g_rt_sys.bytes == 0;
}

/* rt_demote refuses to allocate into a system heap whose usage plus the target would cross its budget less trim_head.
   1 when it returned 0 with a reason and did not try to move the image */
static int rt_demote_stops_at_sys_budget(void) {
    g_vk.mem_model = VK_MEM_SMALL_BAR;
    g_vk_present += 1000;
    Surf* s = target(0x78000000, 16 << 20, 1);
    s->mem = 1;
    g_rt_sys.heap = 1;
    g_sys_budget = 16ull << 30;
    g_sys_use = g_sys_budget - trim_head(g_sys_budget) - (4ull << 20); /* 4 MiB under cap, target is 16 MiB */
    const char* no = "";
    const u64 moved = rt_demote(s, &no);
    const int stopped = moved == 0 && no[0] != 0 && !s->demoted;
    s->mem = 0;
    surf_evict(s);
    CHECK(targets_held() == 0);
    return stopped;
}

/* Level 1 (render targets): a lossy render target whose guest texture object ended goes in vk_mem_trim before an
   older live-object render target. 1 when ended went and live stayed */
static int ended_rt_reclaimed_before_live(void) {
    g_vk.mem_model = VK_MEM_UMA; /* no demotion, so lossy targets evict directly */
    g_vk_present += 1000;
    Surf* live = target(0x79000000ull, 16 << 20, 1);
    live->last_used = g_vk_present - 600;
    Surf* ended = target(0x79800000ull, 16 << 20, 2);
    ended->last_used = g_vk_present - 10;
    g_tex_objs.added = 2;
    g_tex_objs.live[0] = 0x79000000ull;
    g_tex_objs.nlive = 1;
    g_tex_objs.ended[0] = 0x79800000ull;
    g_tex_objs.nended = 1;
    g_use = 0;
    vk_surf_ended_frame(); /* the cache learns of the end while memory has room */
    g_vk_present += 3;
    /* past steps[2]'s mark (budget - head) by 8 MiB: taking ended (16 MiB) satisfies want and leaves live alone */
    g_use = g_budget - trim_head(g_budget) + (8ull << 20);
    g_deferred = 0;
    vk_mem_trim(0, 0);
    const int ok = targets_held() == 1 && !g_rows.lists[VK_LRU_RT].head &&
                   g_rows.surf[g_rows.lists[VK_LRU_RT + LRU_AGED].head] == live && g_deferred == 16 << 20;
    if (!ok)
        fprintf(stderr, "ended_rt_reclaimed_before_live: %u held, deferred %llu MB\n", targets_held(),
                (unsigned long long)(g_deferred >> 20));
    while (g_rows.lists[VK_LRU_RT].head) surf_evict(g_rows.surf[g_rows.lists[VK_LRU_RT].head]);
    while (g_rows.lists[VK_LRU_RT + LRU_AGED].head) surf_evict(g_rows.surf[g_rows.lists[VK_LRU_RT + LRU_AGED].head]);
    g_tex_objs.added = g_tex_objs.nlive = g_tex_objs.nended = 0;
    g_vk.mem_model = VK_MEM_SMALL_BAR;
    return ok;
}

/* Level 1 (buffers): vk_mem_trim reclaims ended (unmapped) own buffers before touching live textures. 1 when ended
   buffers satisfied the trim and the live texture stayed */
static int ended_buf_reclaimed_first(void) {
    g_vk_present += 1000;
    Surf* t = calloc(1, sizeof *t);
    CHECK(t);
    t->gva = 0x7a000000ull;
    t->size = t->bytes = 16 << 20;
    row_take(t);
    lru_add(t, VK_LRU_TEX);
    t->last_used = g_vk_present - 100;
    g_tex_objs.buf_ended_avail = 256ull << 20;
    g_tex_objs.buf_ended_calls = 0;
    g_use = g_budget - trim_head(g_budget) + (8ull << 20);
    g_deferred = 0;
    vk_mem_trim(0, 0);
    const int ok = g_tex_objs.buf_ended_calls > 0 && g_rows.lists[VK_LRU_TEX].head == t->row;
    if (!ok)
        fprintf(stderr, "ended_buf_reclaimed_first: calls=%u tex_kept=%d\n", g_tex_objs.buf_ended_calls,
                g_rows.lists[VK_LRU_TEX].head == t->row);
    while (g_rows.lists[VK_LRU_TEX].head) surf_evict(g_rows.surf[g_rows.lists[VK_LRU_TEX].head]);
    g_tex_objs.buf_ended_avail = 0;
    g_tex_objs.buf_ended_calls = 0;
    return ok;
}

/* Level 2 across types: under system-heap pressure, a lossless demoted texture goes before an older live-object
   demoted render target. 1 when the demoted texture went and the live-object target stayed */
static int sys_pressure_evicts_demoted_tex_before_live_rt(void) {
    g_vk_present += 1000;
    Surf* rt = target(0x7b000000ull, 4 << 20, 1);
    rt->last_used = g_vk_present - 500;
    rt->demoted = 1;
    rt->sys_bytes = 4u << 20;
    vk_surf_bytes(rt, 0);
    g_rt_sys.n++;
    g_rt_sys.bytes += 4u << 20;
    Surf* t = calloc(1, sizeof *t);
    CHECK(t);
    t->gva = 0x7b400000ull;
    t->size = 4u << 20;
    t->demoted = 1;
    t->sys_bytes = 4u << 20;
    g_rt_sys.tex_n++;
    row_take(t);
    lru_add(t, VK_LRU_TEX);
    t->last_used = g_vk_present - 50;
    g_tex_objs.added = 2;
    g_tex_objs.live[0] = 0x7b000000ull;
    g_tex_objs.live[1] = 0x7b400000ull;
    g_tex_objs.nlive = 2;
    g_rt_sys.heap = 1;
    g_sys_budget = 16ull << 30;
    g_sys_use = (14ull << 30) + (2u << 20); /* 2 MiB over sys_high: the 4 MiB demoted texture covers it */
    g_use = g_budget;
    const u64 lost = g_rt_sys.lost;
    vk_rt_sys_frame();
    const int ok = !g_rows.lists[VK_LRU_TEX].head && targets_held() == 1 && g_rt_sys.lost == lost && rt->demoted;
    if (!ok)
        fprintf(stderr, "sys_pressure_evicts_demoted_tex_before_live_rt: tex_left=%u rt_held=%u lost=%llu\n",
                g_rows.lists[VK_LRU_TEX].head != 0, targets_held(), (unsigned long long)(g_rt_sys.lost - lost));
    while (g_rows.lists[VK_LRU_TEX].head) surf_evict(g_rows.surf[g_rows.lists[VK_LRU_TEX].head]);
    while (g_rows.lists[VK_LRU_RT].head) surf_evict(g_rows.surf[g_rows.lists[VK_LRU_RT].head]);
    g_tex_objs.added = g_tex_objs.nlive = 0;
    return ok;
}

/* Level 3 (bandwidth pressure): when GPU reads of demoted surfaces cross the per-frame PCIe bandwidth mark (32 MiB),
   the most-read demoted surface promotes first (even when another sits at the MRU tail) and the per-present promotion
   cap expands past 16 MiB. 1 when the 24 MiB heavily-read texture promoted instead of the 8 MiB tail texture */
static int sys_bw_promotes_most_read(void) {
    g_settings.texture_system_memory = 1;
    g_vk_present += 1000;
    Surf* heavy = calloc(1, sizeof *heavy);
    Surf* light = calloc(1, sizeof *light);
    CHECK(heavy && light);
    heavy->gva = 0x7c000000ull;
    heavy->size = heavy->sys_bytes = 24u << 20;
    heavy->demoted = 1;
    g_rt_sys.tex_n++;
    row_take(heavy);
    lru_add(heavy, VK_LRU_TEX);
    light->gva = 0x7e000000ull;
    light->size = light->sys_bytes = 8u << 20;
    light->demoted = 1;
    g_rt_sys.tex_n++;
    row_take(light);
    lru_add(light, VK_LRU_TEX);
    g_sys_use = 2ull << 30; /* headroom on system heap so only promotion runs */
    /* touch heavy 20 times, then light once at the tail: total unique bytes read this present = 32 MiB */
    for (int i = 0; i < 20; i++) vk_surf_touch(heavy);
    vk_surf_touch(light);
    /* VRAM room for 24 MiB only: heavy (24 MiB) fits alone; if light went first, only 16 MiB would remain */
    g_use = g_budget - trim_head(g_budget) - (24ull << 20);
    g_releasing = 0;
    g_tex_objs.promote_ok = 1;
    g_tex_objs.npromoted = 0;
    vk_tex_promote_frame();
    const int ok = g_tex_objs.npromoted == 1 && g_tex_objs.promoted[0] == heavy && !heavy->demoted && light->demoted;
    if (!ok)
        fprintf(stderr, "sys_bw_promotes_most_read: npromoted=%u first_is_heavy=%d heavy_dem=%u light_dem=%u\n",
                g_tex_objs.npromoted, g_tex_objs.npromoted && g_tex_objs.promoted[0] == heavy, heavy->demoted,
                light->demoted);
    while (g_rows.lists[VK_LRU_TEX].head) surf_evict(g_rows.surf[g_rows.lists[VK_LRU_TEX].head]);
    g_tex_objs.promote_ok = 0;
    g_tex_objs.npromoted = 0;
    g_settings.texture_system_memory = 0;
    return ok;
}

/* Size-bucket PCIe telemetry: vk_bus_bucket boundary values and per-bucket GPU read counting/reset in vk_sys_bw_stats.
   1 when every boundary matched and per-bucket touches and unique bytes matched then cleared on read */
static int sys_bus_buckets(void) {
    CHECK(vk_bus_bucket(0) == VK_BUS_LT64K && vk_bus_bucket((64ull << 10) - 1) == VK_BUS_LT64K);
    CHECK(vk_bus_bucket(64ull << 10) == VK_BUS_LT1M && vk_bus_bucket((1ull << 20) - 1) == VK_BUS_LT1M);
    CHECK(vk_bus_bucket(1ull << 20) == VK_BUS_LT16M && vk_bus_bucket((16ull << 20) - 1) == VK_BUS_LT16M);
    CHECK(vk_bus_bucket(16ull << 20) == VK_BUS_GE16M && vk_bus_bucket(64ull << 20) == VK_BUS_GE16M);
    VkSysBwStats bw;
    vk_sys_bw_stats(&bw); /* clear prior test activity */
    g_vk_present += 10;
    static const u64 sizes[VK_BUS_BUCKETS] = {32u << 10, 256u << 10, 4u << 20, 20u << 20};
    Surf* s[VK_BUS_BUCKETS];
    for (u32 i = 0; i < VK_BUS_BUCKETS; i++) {
        s[i] = calloc(1, sizeof *s[i]);
        CHECK(s[i]);
        s[i]->gva = 0x80000000ull + (u64)i * 0x2000000ull;
        s[i]->size = s[i]->sys_bytes = sizes[i];
        s[i]->demoted = 1;
        g_rt_sys.tex_n++;
        row_take(s[i]);
        lru_add(s[i], VK_LRU_TEX);
    }
    for (int k = 0; k < 5; k++) vk_surf_touch(s[0]);
    for (u32 i = 1; i < VK_BUS_BUCKETS; i++) vk_surf_touch(s[i]);
    vk_sys_bw_stats(&bw);
    const int counted = bw.bucket_touches[0] == 5 && bw.bucket_touches[1] == 1 && bw.bucket_touches[2] == 1 &&
                        bw.bucket_touches[3] == 1 && bw.bucket_unique_bytes[0] == sizes[0] &&
                        bw.bucket_unique_bytes[1] == sizes[1] && bw.bucket_unique_bytes[2] == sizes[2] &&
                        bw.bucket_unique_bytes[3] == sizes[3];
    vk_sys_bw_stats(&bw);
    const int cleared = !bw.bucket_touches[0] && !bw.bucket_touches[1] && !bw.bucket_touches[2] &&
                        !bw.bucket_touches[3] && !bw.bucket_unique_bytes[0] && !bw.bucket_unique_bytes[3];
    while (g_rows.lists[VK_LRU_TEX].head) surf_evict(g_rows.surf[g_rows.lists[VK_LRU_TEX].head]);
    return counted && cleared;
}

int main(void) {
    g_budget = 1ull << 30;
    g_vk_present = 1000;
    /* for headroom where the driver pages: kept a second and eight presents (68 presents here) */
    CHECK(!trim_takes(1, 0, 5) && !trim_takes(1, 0, 20) && !trim_takes(1, 0, 67) && trim_takes(1, 0, 69));
    CHECK(trim_takes(1, 1, 5) && !trim_takes(1, 1, 1)); /* for an allocation: taken after two presents */
    /* where the driver does not page: taken after two presents */
    CHECK(trim_takes(0, 0, 5) && !trim_takes(0, 0, 1) && trim_takes(0, 1, 5));
    /* the caches count room in the blocks, but the heap itself is at its budget: taken after two presents, the hold
       of a second and eight presents aside; with the heap under the trim's start, nothing runs */
    CHECK(heap_full_takes(1, 5) && !heap_full_takes(1, 1) && !heap_full_takes(0, 5) && !heap_full_takes(0, 69));
    /* a texture that took targets' pixels goes once targets drawn after the take cover all its memory: one target, or
       two that meet; not where they leave a gap or one was drawn before the take, nor one loaded from memory alone
       (lossless anyway: the trim's steps take it) */
    const Over whole[] = {{0x50000000, 1 << 20, 11}}, two[] = {{0x4ff00000, 0x180000, 11}, {0x50080000, 1 << 20, 12}},
               gap[] = {{0x50000000, 0x80000, 11}, {0x50090000, 1 << 20, 12}}, before[] = {{0x50000000, 1 << 20, 9}},
               mixed[] = {{0x50000000, 0x80000, 9}, {0x50080000, 0x80000, 11}};
    CHECK(tex_superseded_takes(1, whole, 1) == 1 && tex_superseded_takes(1, two, 2) == 1);
    CHECK(tex_superseded_takes(1, gap, 2) == 0 && tex_superseded_takes(1, before, 1) == 0);
    CHECK(tex_superseded_takes(1, mixed, 2) == 0 && tex_superseded_takes(0, whole, 1) == 0);
    /* a target with pixels memory lacks: kept for its age, taken by the pressure, where it can't move */
    CHECK(aside_or_lost(VK_MEM_UMA) && aside_or_lost(VK_MEM_SMALL_BAR));
    CHECK(aside_hash());
    CHECK(parked_lands_nothing());
    CHECK(sys_heap_short());
    CHECK(copies_go(0) && copies_go(1) && copies_go(2));
    /* a trim for an allocation with the heap's free space in pieces reaches the lossy steps */
    CHECK(forced_in_pieces());
    CHECK(forced_need_ignores_deferred());
    /* a target set aside lets its copies go, but its save copy */
    CHECK(copies_go_aside());
    /* a trim for headroom with the heap's free space in pieces reaches the trim's steps */
    CHECK(headroom_in_pieces());
    /* a forced trim counts deferred releases in the heap's use */
    CHECK(forced_counts_deferred_heap());
    /* a forced trim takes copies unused for VK_RT_COPY_PRESENTS */
    CHECK(forced_copies_go());
    /* demoted render targets stay with headroom, drop dead-object ones first under pressure, and stop before cap */
    CHECK(sys_dead_before_live());
    CHECK(sys_pressure_scales_and_paces());
    CHECK(rt_demote_stops_at_sys_budget());
    /* 4-level eviction across RTs, buffers and textures, plus PCIe read-bandwidth promotion */
    CHECK(ended_rt_reclaimed_before_live());
    CHECK(ended_buf_reclaimed_first());
    CHECK(sys_pressure_evicts_demoted_tex_before_live_rt());
    CHECK(sys_bw_promotes_most_read());
    CHECK(sys_bus_buckets());
    printf(
        "vk_mem_trim: the trims kept and took the targets drawn over as they should, and the sweep took the textures "
        "whose memory targets drew all over after their take and left the others; a target with pixels memory "
        "lacks stayed for its age and went under the pressure where it can't move, loaded memory again where its bytes "
        "changed meanwhile, targets in system memory went oldest first while the system heap was short, and a parked "
        "target's storage result landed nothing; the copies of a live target that no bind asked for in 600 presents "
        "went under the trims' pressure, the others stayed, and none went with no pressure; a trim for an allocation "
        "took a target with pixels memory lacks though the heap's free space was in pieces, and a target set aside "
        "let its copies go but its save copy\n");
    return 0;
}
/* the pool map's views of a texture whose image changed (vk_rt_host): this test makes no pool map */
void vk_pool_map_swapped(const Surf* t) { (void)t; }
