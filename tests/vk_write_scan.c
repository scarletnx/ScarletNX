/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The scans of written pages on Windows (gpu/vk/buffer.c, WW_SCANS_SERIAL), on any host, with no device and one thread:
   the scanner's loop and the checks run by hand. The scanner starts no scan, and claims no leaf, while a check scans;
   it goes on once the check is done; and a check counts as scanning from before its claims until its scan is done,
   one that fails too */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma GCC diagnostic ignored "-Wmissing-field-initializers" /* buffer.c builds without -Wextra */
#define WW_SCANS_SERIAL 1
#include "nx/gpu/vk/buffer.c"

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ------------------------------------------------ the scans, and the scanner's thread run by hand */
#define LEAVES 4u                 /* the guest memory of the model */
static u32 g_scans[2];            /* the scanner's [0] and the checks' [1] */
static int g_scan_fails;          /* the next scan fails */
static int g_seen_checks[2];      /* g_wcheck_scans at the last scan of each */
static u32 g_yields, g_yield_end; /* the scanner's yields, and the one at which the check it waits for is done */
static jmp_buf g_parked;
int plat_ww_scan(u8* p, u64 size, void (*cb)(u8*, u64)) {
    const int check = t_buffer.wstage == &g_wstage_check;
    (void)p, (void)size, (void)cb;
    g_scans[check]++;
    g_seen_checks[check] = atomic_load_explicit(&g_wcheck_scans, memory_order_acquire);
    return g_scan_fails ? -1 : 0;
}
/* the scanner yields while a check scans: no leaf is its own then */
void plat_yield(void) {
    for (u32 l = 0; l < LEAVES; l++) CHECK(!atomic_load_explicit(&g_wleaf_busy[l], memory_order_relaxed));
    CHECK(++g_yields <= g_yield_end);
    if (g_yields == g_yield_end) atomic_store_explicit(&g_wcheck_scans, 0, memory_order_release);
}
int pc_wait(PCond* c, PMutex* m, int64_t timeout_ns) { /* no new epoch: the scanner's pass is over */
    (void)c, (void)m, (void)timeout_ns;
    longjmp(g_parked, 1);
}
static void scanner_pass(void) {
    if (!setjmp(g_parked)) ww_scanner(NULL);
}

/* ------------------------------------------------ what buffer.c reaches otherwise: not used here */
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(2);
}
void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }
#pragma GCC diagnostic ignored "-Wunused-parameter"
u64 gmmu_translate(u64 gva) { abort(); }
u64 gmmu_run(u64 gva, u64 max, u64* cpu) { abort(); }
u64 g_as_size;
_Atomic(int) g_cs_any;
_Atomic(int) g_gfx_any;
_Atomic(int) g_gfx_img_any;
_Atomic int g_gpu_releases;
u8* g_mem;
OwnState g_own;
PendPages g_pend_any;
Settings g_settings;
ShCaps g_spirv_translate;
VkState g_vk;
u64 g_vk_dispatches;
u64 g_vk_drawn;
u32 g_vk_present;
VkResourceStats g_vk_resource_stats;
u8* gmem_host(u64 gva, u64 size) { abort(); }
void gmem_need(u64 gva, u64 size) { abort(); }
u8* gmem_ptr(u64 gva, u64 size) { abort(); }
u8* gmem_ptr_untracked(u64 gva, u64 size) { abort(); }
int gmem_read(u64 gva, void* dst, u64 size) { abort(); }
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz) { abort(); }
u64 gpu_mem_generation(void) { abort(); }
void gpu_mem_written_cpu(u64 cpu, u64 size) { abort(); }
void gpu_mem_written_shader(u64 gva, u64 size) { abort(); }
u32 gpu_pools(GpuPool* out, u32 cap) { abort(); }
int mem_alias_any(void) { return 0; }
int mem_alias_clear(u64 a, u64 size) { return 1; }
void mem_alias_peers(u64 a, u64 size, void (*fn)(void*, u64, u64), void* user) {} /* no views */
int mem_pool_any(u64 a, u64 size) { abort(); }
int mem_ww_probe(void (*cb)(u8*, u64)) { return 0; }

u64 own_first_trapped(u64 addr, u64 size) { abort(); }
int own_hash(u64 addr, u64 size, u64* h) { abort(); }
u32 own_lookup(u64 addr, u64 size, OwnRange* out, u32 max) { abort(); }
int own_traps(void) { abort(); }
void pc_init(PCond* c) {}
void pc_signal(PCond* c) { abort(); }
void pend_add_pages(PendPages* t, u64 first, u64 last, int delta) { abort(); }
int pend_hit(const PendPages* t, u64 gva, u64 size) { abort(); }
u8* plat_alias_mirror(void) { abort(); }
uint64_t plat_ns(void) { return 0; }
int plat_thread(PThreadFn fn, void* arg, size_t stack) { return -1; } /* scanner_pass runs it */
int plat_vm_read(const void* p, void* dst, u64 size) { abort(); }
void plat_ww_cool(u8* p, u64 size, void (*cb)(u8*, u64)) { abort(); }
int plat_ww_hot_in(const u8* p, u64 size) { abort(); }
void pm_lock(PMutex* m) {}
void pm_unlock(PMutex* m) {}
void rec_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, u32 n, const VkBufferCopy* r) { abort(); }
void rec_CmdFillBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkDeviceSize size, u32 data) { abort(); }
void rec_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* d) { abort(); }
void snx_thread_name(const char* name) {}
void vk_all_barrier(VkCommandBuffer cb) { abort(); }
void vk_all_barrier_by(int site, VkCommandBuffer cb) { abort(); }
VkResult vk_buffer_create(const VkBufferCreateInfo* info, const DevMemWant* allocation, VkBuffer* buffer,
                          DevMem* memory, DevMemInfo* details) {
    abort();
}
VkCommandBuffer vk_cmd(void) { abort(); }
VkCommandBuffer vk_cmd_upload(int gpu) { abort(); }
u64 vk_compute_adds(void) { abort(); }
int vk_compute_gpu_src(u64 gva, u64 len, VkBuffer* buf, u64* off) { abort(); }
void vk_compute_mem_need(u64 a, u64 n) { abort(); }
int vk_compute_pending(u64 a, u64 n) { abort(); }
void vk_defer_buffer(VkBuffer b, DevMem a) { abort(); }
void vk_defer_bytes(u64 bytes) { abort(); }
VkResult vk_devmem_invalidate(DevMem mem, u64 off, u64 size) { abort(); }
void vk_finish_at(const char* file, int line) { abort(); }
void vk_frame_hold(u64 frame_id) { abort(); }
u64 vk_frame_id(void) { abort(); }
int vk_gfx_image_pending(u64 gva, u64 size) { abort(); }
void vk_gfx_image_read_need(u64 gva, u64 size) { abort(); }
void vk_gfx_image_report(void) { abort(); }
void vk_guest_read(u64 gva, u64 size, u8* host, VkBuffer buf, u64 at) { abort(); }
int vk_guest_read_ahead(u64 gva, u64 size, VkBuffer buf, u64 at) { abort(); }
int vk_guest_read_device(u64 gva, u64 size, VkBuffer buf, u64 at) { abort(); }
u64 vk_hash(const void* p, u64 size) { abort(); }
u64 vk_hash_copy(void* dst, const void* src, u64 size) { abort(); }
void vk_land_through(u64 value) { abort(); }
u64 vk_last_submit(void) { abort(); }
void vk_ledger_heap(u32 owner, i64 delta) { abort(); }
void vk_lock(void) { abort(); }
void vk_mem_track(DevMem a, u32 owner) { abort(); }
void vk_mem_use(u64* usage, u64* budget) { abort(); }
void vk_mirror_done(u64 done) { abort(); }
void vk_mirror_harvest(VkCommandBuffer cb, u64 submit) { abort(); }
int vk_mirror_holds(u64 gva, u64 len) { abort(); }
int vk_mirror_holds_at(u64 cpu, u64 len) { abort(); }
int vk_mirror_owns_any(void) { return 0; } /* no mirror here */
int vk_mirror_slice_at(u64 gva, u64 cpu, u64 len, VkBuffer* buf, u64* off) { abort(); }
u64 vk_mirror_idle(u32 presents) { abort(); }
void vk_mirror_mem_need(u64 gva, u64 len) { abort(); }
int vk_mirror_pending(u64 gva, u64 len) { abort(); }
u64 vk_mirror_released(void) { abort(); }
int vk_mirror_slice(u64 gva, u64 len, VkBuffer* buf, u64* off) { abort(); }
void vk_mirror_stats(u64* granules, u64* harvests, u64* pages, u64* landed, u64* overflows, u32* room) { abort(); }
void vk_mirror_trim(void) { abort(); }
u64 vk_mirror_write_gen(void) { abort(); }
int vk_ok_(VkResult r, const char* what, const char* file, int line) { abort(); }
int vk_own_cpu_read(u64 cpu, u64 size, u32 presents) { abort(); }
void vk_own_publish(u64 cpu, u64 size, VkOwnRes* r, u64 off) { abort(); }
VkOwnRes* vk_own_res_new(u64 size, u32 by) { abort(); }
void vk_own_res_ref(VkOwnRes* r) { abort(); }
void vk_own_res_unref(VkOwnRes* r) { abort(); }
void vk_own_res_wrote(VkOwnRes* r) { abort(); }
void vk_pages_forget_all(void) { abort(); }
void vk_pages_frame(void) { abort(); }
int vk_pages_get(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off) { abort(); }
int vk_pages_keep(void) { abort(); }
void vk_pages_log(void) { abort(); }
int vk_pages_on(void) { abort(); }
void vk_pages_order_request(void) { abort(); }
void vk_pages_ordered(void) { abort(); }
void vk_pages_read(u64 gva, u64 len) { abort(); }
u64 vk_pages_reclaim(u32 before, u64 want) { abort(); }
void vk_pages_submit(void) { abort(); }
int vk_pages_write(u64 gva, u64 len, u64 align, int ordered, VkBuffer* buf, u64* off) { abort(); }
u64 vk_pages_written(void) { abort(); }
u32 vk_present_before(u64 ago_ns, u32 presents) { abort(); }
int vk_query_counts(VkBuffer buf, u64 off, const SamplesCount* counts, u32 n) { abort(); }
u64 vk_range_inside(const VkRange* r, u32 n, u64 start, u64 len) { abort(); }
u32 vk_range_join(VkRange* r, u32 n) { abort(); }
void vk_range_shape(VkRange* r, u32 n, const u64 gran[2], VkRangeShape* out) { abort(); }
int vk_release_pending(u64 gva, u64 len) { abort(); }
int vk_release_read_counts(u64 gva, void* dst, u64 len, SamplesCount* counts, u32 max, u32* n) { abort(); }
void vk_render_end_at(const char* file, int line) { abort(); }
u64 vk_sample_hash(u64 gva, u64 len) { abort(); }
u64 vk_shape_bucket_floor(u32 bucket) { abort(); }
u8* vk_stage(u64 size, u64 align, VkBuffer* buf, u64* off) { abort(); }
u64 vk_stage_align(void) { abort(); }
u8* vk_stage_back(u64 size, VkBuffer* buf, u64* off, DevMem* mem) { abort(); }
int vk_stage_device_at(const char* file, int line, u64 size, u64 align, VkBuffer* buf, u64* off) { abort(); }
u8* vk_stage_output(u64 size, u64 align, VkBuffer* buf, u64* off, DevMem* mem) { abort(); }
void vk_stage_report(void) { abort(); }
u8* vk_stage_upload(u64 size, u64 align, VkBuffer* buf, u64* off) { abort(); }
u8* vk_stream_upload(u64 size, u64 align, VkBuffer* buf, u64* off) { abort(); }
u64 vk_submit_number(void) { abort(); }
int vk_uma(void) { return 0; }
void vk_uma_forget_all(void) { abort(); }
void vk_uma_frame(void) { abort(); }
void vk_uma_frame_done(u64 frame) { abort(); }
int vk_uma_get(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off) { abort(); }
int vk_uma_gpu_write(u64 cpu, u64 len, VkBuffer* buf, u64* off) { abort(); }
void vk_uma_gpu_wrote(u64 cpu, u64 len) { abort(); }
void vk_uma_host_barrier(VkCommandBuffer cb, u64 frame) { abort(); }
u64 vk_uma_import_in(u64 cpu, u64 end, u64* hi) { return *hi = end; } /* no imports */
u64 vk_uma_import_part(u64 cpu, u64 len, int* held) { abort(); }
int vk_uma_imports_dear(void) { abort(); }
int vk_uma_pending(u64 gva, u64 len) { abort(); }
int vk_uma_write(u64 gva, u64 len, u64 align, int ordered, VkBuffer* buf, u64* off) { abort(); }
void vk_uma_order_request(void) { abort(); }
void vk_uma_ordered(void) { abort(); }
void vk_uma_writes_abort(void) { abort(); }
void vk_uma_writes_commit(void) { abort(); }
void vk_unlock(void) { abort(); }
void vk_wait_submit(u64 value) { abort(); }
u64 vk_wlog_again(void) { abort(); }
int vk_wlog_merge(VkWriteLog* l, u32 part, u32 from, u32 to, VkBuffer src, u64 src_off, VkBuffer dst, u64 dst_off) {
    abort();
}
void vk_wlog_ref(VkWriteLog* l) { abort(); }
u32 vk_wlog_runs(VkWriteLog* l, u32 part, u32 size, VkRunFn fn, void* user) { abort(); }
void vk_wlog_stats(u64* logs, u64* bytes, u64* in_use, u64* windows) { abort(); }
void vk_wlog_trim(void) { abort(); }
void vk_wlog_unref(VkWriteLog* l) { abort(); }
void vk_write_changed_runs(u64 addr, const u8* res, const u8* org, u32 size, VkChangedRun run, void* user) { abort(); }
u32 vk_write_runs(u64 addr, const u8* res, const VkRun* runs, u32 n, u32 since) { abort(); }
void vk_writeback_diag(u64* cmp, u64* chg) { abort(); }

/* ------------------------------------------------ the checks */
#define LEAF(n) ((u64)(n) << WW_LEAF_SHIFT)
static int checked(u32 leaf) { return vk_written_since(g_mem + LEAF(leaf), 4096, 0); }
static void counts(u32 scanner, u32 check) {
    CHECK(g_scans[0] == scanner && g_scans[1] == check);
    g_scans[0] = g_scans[1] = 0;
}

int main(void) {
    g_as_size = LEAF(LEAVES);
    g_mem = calloc(1, g_as_size);
    CHECK(g_mem);

    /* a check scans its leaf, counted while it does and not after */
    vk_ww_new_epoch();
    CHECK(checked(0) == 0);
    counts(0, 1);
    CHECK(g_seen_checks[1] == 1 && atomic_load_explicit(&g_wcheck_scans, memory_order_acquire) == 0);

    /* with no check scanning, the scanner scans the leaf in the next epoch with no yield, and the check then scans
       nothing */
    vk_ww_new_epoch();
    scanner_pass();
    counts(1, 0);
    CHECK(!g_yields && !g_seen_checks[0]);
    CHECK(checked(0) == 0);
    counts(0, 0);

    /* with one scanning, the scanner yields until it is done, having claimed nothing, and scans then */
    vk_ww_new_epoch();
    atomic_store_explicit(&g_wcheck_scans, 1, memory_order_release);
    g_yield_end = 3;
    scanner_pass();
    counts(1, 0);
    CHECK(g_yields == 3 && !g_seen_checks[0]);

    /* the next leaf's scan waits as the first one's did */
    vk_ww_new_epoch();
    CHECK(checked(0) == 0 && checked(2) == 0); /* two the scanner can't take in one run */
    counts(0, 2);
    vk_ww_new_epoch();
    g_yields = 0;
    atomic_store_explicit(&g_wcheck_scans, 1, memory_order_release);
    scanner_pass();
    counts(2, 0);
    CHECK(g_yields == 3);

    /* a check whose scan fails lets the scanner go too */
    vk_ww_new_epoch();
    g_scan_fails = 1;
    CHECK(checked(0) == -1);
    counts(0, 1);
    CHECK(g_seen_checks[1] == 1 && atomic_load_explicit(&g_wcheck_scans, memory_order_acquire) == 0);
    for (u32 l = 0; l < LEAVES; l++) CHECK(!atomic_load_explicit(&g_wleaf_busy[l], memory_order_relaxed));
    puts("vk_write_scan: the scanner starts no scan while a check scans");
    return 0;
}
