/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GPU mirror's bookkeeping on the CPU (vk/mirror.c), with no device: the ranges its programs write and their
   pending counts (vk_mirror_wrote, the real pending_pages.c), and the landing of a page's entry (land_entry): which
   owners and render targets it asks for, and the bytes it puts in guest memory. Everything else mirror.c calls is a
   stub below */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma GCC diagnostic ignored "-Wmissing-field-initializers" /* mirror.c builds without -Wextra */
#include "nx/gpu/vk/mirror.c"

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #x);                                      \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ------------------------------------------------ guest memory and the globals mirror.c reads */
#define GUEST (1u << 20)
static u8 guest[GUEST];
u8* g_mem = guest;
u64 g_as_size = GUEST;
MemLayout g_layout;
OwnState g_own;
_Atomic int g_mir_any;
Settings g_settings;
VkState g_vk;
u32 g_vk_present;
const u32 mirror_fold_spv[1];
const u64 mirror_fold_spv_size = 4;
const u32 mirror_harvest_spv[1];
const u64 mirror_harvest_spv_size = 4;
PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets;
PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
PFN_vkCreateBuffer vkCreateBuffer;
PFN_vkCreateCommandPool vkCreateCommandPool;
PFN_vkCreateDescriptorPool vkCreateDescriptorPool;
PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout;
PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
PFN_vkDestroyBuffer vkDestroyBuffer;
PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool;
PFN_vkEndCommandBuffer vkEndCommandBuffer;
PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
PFN_vkQueueSubmit2 vkQueueSubmit2;
PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets;

/* ------------------------------------------------ what the landing asks of owners and targets, recorded */
static struct {
    u32 acquires, targets_asked, before_writes, written_cpu;
    u64 acquire_at[64], acquire_len[64], before_at[64], before_len[64];
    int targets; /* what vk_rt_pending_cpu answers */
} g_rec;
void own_acquire(u64 addr, u64 size) {
    if (g_rec.acquires < 64) g_rec.acquire_at[g_rec.acquires] = addr, g_rec.acquire_len[g_rec.acquires] = size;
    g_rec.acquires++;
}
int vk_rt_pending_cpu(u64 cpu, u64 size) {
    (void)cpu, (void)size;
    g_rec.targets_asked++;
    return g_rec.targets;
}
void vk_rt_before_write_cpu(u64 cpu, u64 size) {
    if (g_rec.before_writes < 64) g_rec.before_at[g_rec.before_writes] = cpu, g_rec.before_len[g_rec.before_writes] = size;
    g_rec.before_writes++;
}
void gpu_mem_written_cpu(u64 cpu, u64 size) { (void)cpu, (void)size, g_rec.written_cpu++; }

/* ------------------------------------------------ the rest: nothing these cases reach does more */
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}
void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }
void gmmu_aliases(u64 cpu, u64 size, void (*fn)(void* ctx, u64 gva, u64 n), void* ctx) { (void)cpu, (void)size, (void)fn, (void)ctx; }
u64 gmmu_run(u64 gva, u64 max, u64* cpu) { return *cpu = gva, max; }
void gmmu_pieces(u64 gva, u64 size, void (*fn)(void* ctx, u64 gva, u64 cpu, u64 n), void* ctx) { (void)gva, (void)size, (void)fn, (void)ctx; }
void gpu_mem_dirty_runs(u64 since, u64 a, u64 sz, void (*fn)(void* ctx, u64 at, u64 n), void* ctx) { (void)since, (void)a, (void)sz, (void)fn, (void)ctx; }
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz) { return (void)since, (void)a, (void)sz, 0; }
u64 gpu_mem_generation(void) { return 1; }
void vk_submit_progress(u64* submitted, u64* done) { *submitted = *done = 0; }
void vk_land_through(u64 value) { (void)value; }
/* owned.c's: the stages a buffer barrier names */
VkPipelineStageFlags2 vk_buffer_stages(void) { return VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; }
int own_hash(u64 addr, u64 size, u64* h) { return (void)addr, (void)size, (void)h, 0; }
u32 own_lookup(u64 addr, u64 size, OwnRange* out, u32 max) { return (void)addr, (void)size, (void)out, (void)max, 0; }
u32 own_stage(u64 addr, u64 size, u8* dst, OwnRange* out, u32 max) { return (void)addr, (void)size, (void)dst, (void)out, (void)max, 0; }
int own_traps(void) { return 1; }
void rec_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, u32 first, u32 n,
                               const VkDescriptorSet* sets, u32 ndyn, const u32* dyn) { (void)cb, (void)bp, (void)layout, (void)first, (void)n, (void)sets, (void)ndyn, (void)dyn; }
void rec_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline pipe) { (void)cb, (void)bp, (void)pipe; }
void rec_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, u32 n, const VkBufferCopy* r) { (void)cb, (void)src, (void)dst, (void)n, (void)r; }
void rec_CmdDispatch(VkCommandBuffer cb, u32 x, u32 y, u32 z) { (void)cb, (void)x, (void)y, (void)z; }
void rec_CmdFillBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkDeviceSize size, u32 data) { (void)cb, (void)buf, (void)off, (void)size, (void)data; }
void rec_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* d) { (void)cb, (void)d; }
void rec_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, u32 offset, u32 size,
                          const void* data) { (void)cb, (void)layout, (void)stages, (void)offset, (void)size, (void)data; }
VkResult snx_vk_compute_pipeline(VkDevice dev, VkPipelineCache cache, VkPipelineLayout layout, const uint32_t* words,
                                 size_t size, VkPipeline* out) { return (void)dev, (void)cache, (void)layout, (void)words, (void)size, (void)out, VK_ERROR_UNKNOWN; }
VkResult vk_buffer_create(const VkBufferCreateInfo* info, const DevMemWant* allocation, VkBuffer* buffer,
                          DevMem* memory, DevMemInfo* details) { return (void)info, (void)allocation, (void)buffer, (void)memory, (void)details, VK_ERROR_UNKNOWN; }
u64 vk_buffer_sparse_bind(VkBuffer buf, const VkSparseMemoryBind* bind, DevMem gone, u64 size, int readers) { return (void)buf, (void)bind, (void)gone, (void)size, (void)readers, 0; }
VkCommandBuffer vk_cmd(void) { return VK_NULL_HANDLE; }
VkDescriptorSet vk_desc_alloc(VkDescriptorSetLayout layout) { return (void)layout, VK_NULL_HANDLE; }
VkResult vk_devmem_alloc(const VkMemoryRequirements* req, const DevMemWant* want, DevMem* mem, DevMemInfo* info) { return (void)req, (void)want, (void)mem, (void)info, VK_ERROR_UNKNOWN; }
void vk_devmem_buffer_destroy(VkBuffer buf, DevMem mem) { (void)buf, (void)mem; }
void vk_devmem_free(DevMem mem) { (void)mem; }
VkResult vk_devmem_invalidate(DevMem mem, u64 off, u64 size) { return (void)mem, (void)off, (void)size, VK_SUCCESS; }
void vk_finish_at(const char* file, int line) { (void)file, (void)line; }
void vk_finish_letting_go_at(const char* file, int line) { (void)file, (void)line; }
void vk_gpuprof_tag(u64 key) { (void)key; }
int vk_host_bind_sparse(VkHostTimeline* t, const VkBindSparseInfo* bi) { return (void)t, (void)bi, 0; }
u64 vk_host_next(VkHostTimeline* t) { return (void)t, 0; }
int vk_host_wait(const VkHostTimeline* t, u64 value) { return (void)t, (void)value, 0; }
u64 vk_last_submit(void) { return 0; }
void vk_ledger_heap(u32 owner, i64 delta) { (void)owner, (void)delta; }
void vk_lock(void) {}
void vk_unlock(void) {}
void vk_mem_track(DevMem a, u32 owner) { (void)a, (void)owner; }
void vk_mem_use(u64* usage, u64* budget) { *usage = 0, *budget = 1; }
int vk_ok_(VkResult r, const char* what, const char* file, int line) { return (void)what, (void)file, (void)line, r == VK_SUCCESS; }
int vk_own_cpu_read(u64 cpu, u64 size, u32 presents) { return (void)cpu, (void)size, (void)presents, 0; }
void vk_own_publish_submitted(u64 cpu, u64 size, VkOwnRes* r, u64 off) { (void)cpu, (void)size, (void)r, (void)off; }
VkOwnRes* vk_own_res_borrow(VkBuffer buf) { return (void)buf, NULL; }
void vk_own_res_unref(VkOwnRes* r) { (void)r; }
VkPipelineCache vk_pipeline_cache(void) { return VK_NULL_HANDLE; }
void vk_queue_lock(VkQueue q) { (void)q; }
void vk_queue_unlock(VkQueue q) { (void)q; }
int vk_rec_unsafe(void) { return 0; }
void vk_render_end_at(const char* file, int line) { (void)file, (void)line; }
void vk_result_begin(void) {}
void vk_result_end(void) {}
u8* vk_stage_back(u64 size, VkBuffer* buf, u64* off, DevMem* mem) { return (void)size, (void)buf, (void)off, (void)mem, NULL; }
u8* vk_stream_upload(u64 size, u64 align, VkBuffer* buf, u64* off) { return (void)size, (void)align, (void)buf, (void)off, NULL; }
void vk_submit(VkSemaphore wait, VkPipelineStageFlags2 wait_stage, VkSemaphore signal) { (void)wait, (void)wait_stage, (void)signal; }
int vk_uma(void) { return 0; }
int vk_uma_storage_copied(void) { return 0; }
[[noreturn]] void vk_lost_stop(const char* msg) {
    fprintf(stderr, "mirror_landing: %s\n", msg);
    exit(1);
}
void vk_written_runs(const u8* host, u64 len, u32 since, void (*fn)(void*, u64, u64), void* user) { (void)host, (void)len, (void)since, (void)fn, (void)user; }
int vk_written_since(const u8* host, u64 len, u32 since) { return (void)host, (void)len, (void)since, 0; }
u32 vk_ww_epoch(void) { return 1; }
u32 vk_ww_now(void) { return 1; }

/* ------------------------------------------------ the cases */
/* every page's pending count added up (PendPages.total): a page that two listed ranges hold counts twice */
static u64 pending_pages(void) { return g_mir_pend.total; }

/* A written range that one of the submission's last ranges holds already is neither listed nor counted: its pages
   stay pending through the range that holds them. One that reaches past them is, as a range that meets none, and so
   is one that two ranges hold between them but neither alone, and one that only a range nine or more back holds
   (vk_mirror_wrote looks at the last 8). The counts are then taken as vk_mirror_harvest takes them, a model of it:
   each listed range once */
static void check_wrote_held(void) {
    vk_mirror_wrote(0x10000, 0x8000); /* pages 0x10 to 0x17 */
    CHECK(g_mir_nnow == 1 && pending_pages() == 8);
    vk_mirror_wrote(0x12000, 0x2000); /* inside it */
    vk_mirror_wrote(0x10000, 0x8000); /* the same again */
    vk_mirror_wrote(0x17FFF, 1);      /* its last byte */
    CHECK(g_mir_nnow == 1 && pending_pages() == 8);
    vk_mirror_wrote(0x17000, 0x2000); /* past its end: listed and counted */
    CHECK(g_mir_nnow == 2 && pending_pages() == 10);
    vk_mirror_wrote(0x30000, 0x1000); /* elsewhere */
    CHECK(g_mir_nnow == 3 && pending_pages() == 11);
    vk_mirror_wrote(0x17800, 0x1000); /* inside the second, past the first */
    CHECK(g_mir_nnow == 3 && pending_pages() == 11);
    vk_mirror_wrote(0x10000, 0x9000); /* the first and the second together, in neither alone: listed and counted */
    CHECK(g_mir_nnow == 4 && pending_pages() == 20);
    for (u32 k = 0; k < 8; k++) vk_mirror_wrote(0x100000 + (u64)k * 0x10000, 0x1000); /* 8 ranges apart */
    CHECK(g_mir_nnow == 12 && pending_pages() == 28);
    vk_mirror_wrote(0x12000, 0x1000); /* inside the first, 11 back: listed and counted again */
    CHECK(g_mir_nnow == 13 && pending_pages() == 29);
    vk_mirror_wrote(0x170000, 0x1000); /* inside the last of the 8, 2 back: held */
    CHECK(g_mir_nnow == 13 && pending_pages() == 29);
    vk_mirror_wrote(0x110000, 0x1000); /* inside the second of the 8, 8 back: held */
    CHECK(g_mir_nnow == 13 && pending_pages() == 29);
    vk_mirror_wrote(0x100000, 0x1000); /* inside the first of the 8, 9 back: listed and counted */
    CHECK(g_mir_nnow == 14 && pending_pages() == 30);
    for (u32 i = 0; i < g_mir_nnow; i++) mir_pend_pages(g_mir_now[i].first, g_mir_now[i].end - 1, -1);
    g_mir_nnow = 0;
    CHECK(pending_pages() == 0 && !g_mir_any);
}

/* an entry of the harvest's output for the page at cpu: its mask's runs [run[k][0], run[k][1]) of bytes, its data
   the byte index plus fill */
static u32 entry[MIR_ENTRY];
static void entry_make(u64 cpu, const u32 (*run)[2], u32 n, u8 fill) {
    memset(entry, 0, sizeof entry);
    entry[0] = (u32)(cpu >> 12);
    u8* data = (u8*)(entry + 132);
    for (u32 b = 0; b < 4096; b++) data[b] = (u8)(b + fill);
    for (u32 k = 0; k < n; k++)
        for (u32 b = run[k][0]; b < run[k][1]; b++) entry[4 + b / 32] |= 1u << (b % 32);
}
/* guest memory at cpu is fill's bytes over exactly the runs, and its own (0x11) elsewhere in the page */
static int page_is(u64 cpu, const u32 (*run)[2], u32 n, u8 fill) {
    for (u32 b = 0; b < 4096; b++) {
        int in = 0;
        for (u32 k = 0; k < n; k++) in |= b >= run[k][0] && b < run[k][1];
        if (guest[cpu + b] != (in ? (u8)(b + fill) : 0x11)) return 0;
    }
    return 1;
}
static void rec_clear(int targets) {
    memset(&g_rec, 0, sizeof g_rec);
    g_rec.targets = targets;
}

/* A page's entry lands its runs. With no render target pending over the page it lands the page's owners once and asks
   for targets once, not for each run: each run asked both through every GPU address mapping it (43 million asks in
   about 100 s of one game, 612 of them finding a target). With one pending it lands the page's owners once, then asks
   both for each run, as before, as a target's write-back in one run may change the page's owners. A page whose mask is
   empty lands nothing and asks nothing. Each stamps the page written */
static void check_land_entry(void) {
    const u64 cpu = 0x40000;
    const u32 runs[3][2] = {{0, 16}, {100, 260}, {4000, 4096}};
    memset(guest + cpu, 0x11, 4096);
    entry_make(cpu, runs, 3, 0x40);
    rec_clear(0);
    land_entry(entry);
    CHECK(page_is(cpu, runs, 3, 0x40));
    CHECK(g_rec.acquires == 1 && g_rec.acquire_at[0] == cpu && g_rec.acquire_len[0] == 4096);
    CHECK(g_rec.targets_asked == 1 && g_rec.before_writes == 0 && g_rec.written_cpu == 1);
    /* a target pending over the page: each run as before */
    memset(guest + cpu, 0x11, 4096);
    entry_make(cpu, runs, 3, 0x80);
    rec_clear(1);
    land_entry(entry);
    CHECK(page_is(cpu, runs, 3, 0x80));
    CHECK(g_rec.acquires == 1 + 3 && g_rec.targets_asked == 1 && g_rec.before_writes == 3);
    for (u32 k = 0; k < 3; k++) {
        CHECK(g_rec.acquire_at[1 + k] == cpu + runs[k][0] && g_rec.acquire_len[1 + k] == runs[k][1] - runs[k][0]);
        CHECK(g_rec.before_at[k] == cpu + runs[k][0] && g_rec.before_len[k] == runs[k][1] - runs[k][0]);
    }
    CHECK(g_rec.written_cpu == 1);
    /* an empty mask: nothing landed, nothing asked */
    memset(guest + cpu, 0x11, 4096);
    entry_make(cpu, runs, 0, 0xC0);
    rec_clear(1);
    land_entry(entry);
    CHECK(page_is(cpu, runs, 0, 0xC0));
    CHECK(g_rec.acquires == 0 && g_rec.targets_asked == 0 && g_rec.before_writes == 0 && g_rec.written_cpu == 1);
}

int main(void) {
    check_wrote_held();
    check_land_entry();
    printf("mirror_landing: %u checks passed\n", checks);
    return 0;
}
