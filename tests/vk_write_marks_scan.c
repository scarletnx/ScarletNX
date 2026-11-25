/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The pages a game wrote, found from its write marks (gpu/vk/buffer.c, write_marks_scan=1), with no device and no
   host tracking: memory/mem.c's marks modelled as a byte a page that mem_marks_scan reads and clears. A range reads
   as unchanged after its baseline until a page of it is marked, and as written in the next write epoch after a mark,
   its pages besides it still unchanged; a scan takes the marks it reports; a page a trap holds keeps its mark, and
   reads unchanged until the trap lets go of it, as the host's tracking leaves such a page for then.
   --check and --check-uma: write_marks_scan=2 with the host's tracking modelled too, and the GPU's writes told of by the
   watch (unified memory's hazard count holds it there, and tells the check itself). A page the host found written that
   the GPU side wrote is no page with no mark, with either memory model. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma GCC diagnostic ignored "-Wmissing-field-initializers" /* buffer.c builds without -Wextra */
#include "nx/gpu/vk/buffer.c"

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ------------------------------------------------ the marks: a byte a page, as mem.c keeps them below guest memory */
#define LEAVES 2u /* the guest memory of the model: whole leaves, as the scans cover */
#define PAGES  (LEAVES << (WW_LEAF_SHIFT - 12))
static u8 g_marks[PAGES], g_host[PAGES]; /* the marks, and the pages the host's tracking (modelled) has written */
static u64 g_trapped = ~0ull; /* the guest address of the page a trap holds, ~0 for none */
static u32 g_scans;           /* calls of mem_marks_scan */
/* as mem.c's: the runs of marked pages in [a, a + size), each mark taken */
int mem_marks_scan(u64 a, u64 size, void (*cb)(u8*, u64)) {
    g_scans++;
    for (u64 pg = a >> 12, e = (a + size) >> 12; pg < e;) {
        if (!g_marks[pg]) {
            pg++;
            continue;
        }
        u64 run = pg;
        while (pg < e && g_marks[pg]) g_marks[pg++] = 0;
        cb(g_mem + (run << 12), (pg - run) << 12);
    }
    return 0;
}

/* ------------------------------------------------ what buffer.c reaches otherwise: not used here */
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(2);
}
static char g_checked[512]; /* the check's last line */
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    (void)lvl;
    if (!strstr(fmt, "write marks check over")) return;
    va_start(ap, fmt);
    vsnprintf(g_checked, sizeof g_checked, fmt, ap);
    va_end(ap);
}
#pragma GCC diagnostic ignored "-Wunused-parameter"
u64 gmmu_translate(u64 gva) { abort(); }
u64 gmmu_run(u64 gva, u64 max, u64* cpu) { abort(); }
static void (*g_watch)(u64 cpu, u64 n); /* the renderer's watch on GPU-side writes: check mode's, where it is free */
void gpu_set_write_watch(void (*fn)(u64 cpu, u64 n)) { g_watch = fn; }
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
int mem_ww_probe(void (*cb)(u8*, u64)) { return 0; } /* the host tracks (check mode) */
int mem_marks_on(void) { return 1; }

u8 mem_mark_peek(u64 a) { return g_marks[a >> 12]; }
void mem_wrote(u64 a, u64 size) {
    for (u64 pg = a >> 12; pg <= (a + size - 1) >> 12; pg++) g_marks[pg] = 1;
}
u64 own_first_trapped(u64 addr, u64 size) { abort(); }
int own_hash(u64 addr, u64 size, u64* h) { abort(); }
u32 own_lookup(u64 addr, u64 size, OwnRange* out, u32 max) { abort(); }
int own_traps(void) { abort(); }
void pc_init(PCond* c) {}
void pc_signal(PCond* c) { abort(); }
int pc_wait(PCond* c, PMutex* m, int64_t timeout_ns) { abort(); }
void pend_add_pages(PendPages* t, u64 first, u64 last, int delta) { abort(); }
int pend_hit(const PendPages* t, u64 gva, u64 size) { abort(); }
u8* plat_alias_mirror(void) { abort(); }
uint64_t plat_ns(void) { return 0; }
int plat_thread(PThreadFn fn, void* arg, size_t stack) { return -1; } /* no scanner: the checks scan */
int plat_vm_read(const void* p, void* dst, u64 size) { abort(); }
void plat_ww_cool(u8* p, u64 size, void (*cb)(u8*, u64)) { abort(); }
int plat_ww_hot_in(const u8* p, u64 size) { abort(); }
int plat_ww_scan(u8* p, u64 size, void (*cb)(u8*, u64)) { /* the host's, as mem_marks_scan is for the marks */
    for (u64 pg = (u64)(p - g_mem) >> 12, e = ((u64)(p - g_mem) + size) >> 12; pg < e; pg++)
        if (g_host[pg]) g_host[pg] = 0, cb(g_mem + (pg << 12), 4096);
    return 0;
}
int plat_trap_armed(const void* p) { return (u64)((const u8*)p - g_mem) == g_trapped; }
void plat_trap_open_watch(void (*fn)(u8*, u64)) {}
void plat_yield(void) { abort(); }
void pm_lock(PMutex* m) {}
void pm_unlock(PMutex* m) {}
void rec_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, u32 n, const VkBufferCopy* r) { abort(); }
void rec_CmdFillBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkDeviceSize size, u32 data) { abort(); }
void rec_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* d) { abort(); }
void snx_thread_name(const char* name) { abort(); }
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
static int g_uma_on;
int vk_uma(void) { return g_uma_on; }
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
int vk_uma_storage_copied(void) { abort(); }
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
#define P(n) ((u64)(n) << 12)
static int written(u64 a, u64 len, u32 since) { return vk_written_since(g_mem + a, len, since); }

/* check mode: three pages the host found written, one by the GPU's side (no mark of it), one by nothing the marks saw, one
   marked by the game's code: the check's line says 3, 1 with no mark, 1 more the GPU wrote. With unified memory (uma) the
   watch is the hazard count's, which this stands in for */
static int check(int uma) {
    unsigned presents, host = 0, unmarked = 0, more = 0, later = 0, since = 0, queries = 0, stamped = 0;
    g_uma_on = uma;
    g_settings.write_marks_scan = 2;
    CHECK(written(P(10), P(3), 0) == 0);
    CHECK(uma ? !g_watch : g_watch != NULL); /* never taken from the hazard count */
    vk_ww_new_epoch();
    const u32 base = vk_ww_now();
    g_host[10] = g_host[11] = g_host[12] = 1;
    g_marks[12] = 1;
    if (uma)
        vk_ww_gpu_wrote(P(10), 4096);
    else
        g_watch(P(10), 4096);
    vk_ww_new_epoch();
    CHECK(written(P(10), P(3), base) == 1);
    vk_ww_stats(1);
    CHECK(
        sscanf(
            g_checked,
            "vulkan: write marks check over %u presents: %u pages the host found written, %u of them with no mark "
            "(%u marked by a later scan, %u pages with no mark since); %u queries read as written for such a page, %u "
            "of them on a page a GPU-side write stamped; %u more had a GPU-side write or a landing",
            &presents, &host, &unmarked, &later, &since, &queries, &stamped, &more) == 8);
    if (host != 3 || unmarked != 1 || more != 1) fprintf(stderr, "%s", g_checked);
    CHECK(host == 3 && unmarked == 1 && more == 1);
    printf("vk_write_marks_check%s: ok\n", uma ? ", unified memory" : "");
    return 0;
}

int main(int argc, char** argv) {
    g_as_size = (u64)LEAVES << WW_LEAF_SHIFT;
    g_mem = calloc(1, g_as_size);
    CHECK(g_mem);
    if (argc > 1 && !strcmp(argv[1], "--check")) return check(0);
    if (argc > 1 && !strcmp(argv[1], "--check-uma")) return check(1);
    CHECK(argc == 1);
    g_settings.write_marks_scan = 1;
    CHECK(written(P(10), P(2), 0) == 0); /* since 0: whether the tracking works */
    CHECK(g_scans == 1);                 /* the range's leaf, scanned from the marks */

    /* a baseline, then a page marked as the game's code marks it: written in the next epoch, its neighbours not */
    vk_ww_new_epoch();
    u32 base = vk_ww_now();
    CHECK(written(P(10), P(2), base) == 0);
    g_marks[11] = 1;
    vk_ww_new_epoch();
    CHECK(written(P(10), P(2), base) == 1); /* an 8 KiB constant buffer over pages 10 and 11 */
    CHECK(written(P(11) + 0x800, 16, base) == 1);
    CHECK(written(P(10), P(1), base) == 0);
    CHECK(written(P(12), P(4), base) == 0);
    CHECK(!g_marks[11]); /* the scan took it */
    u32 after = vk_ww_now();
    CHECK(written(P(10), P(2), after) == 0); /* a baseline after the write */

    /* the runtime's own writes mark through mem_wrote, and a page in the other leaf is found as well */
    mem_wrote(P(1024 + 5) + 100, 8000); /* pages 1029 and 1030 */
    vk_ww_new_epoch();
    CHECK(written(P(1029), P(1), after) == 1);
    CHECK(written(P(1030), P(1), after) == 1);
    CHECK(written(P(1031), P(1), after) == 0);
    CHECK(written(P(1028), P(1), after) == 0);

    /* a page a trap holds keeps its mark until the trap lets go of it */
    vk_ww_new_epoch();
    u32 held = vk_ww_now();
    CHECK(written(P(20), P(3), held) == 0);
    g_trapped = P(21);
    g_marks[21] = 1;
    vk_ww_new_epoch();
    CHECK(written(P(20), P(3), held) == 0);
    CHECK(g_marks[21]); /* set again for the scan after the trap */
    g_trapped = ~0ull;
    vk_ww_new_epoch();
    CHECK(written(P(20), P(3), held) == 1);
    CHECK(written(P(20), P(1), held) == 0);
    CHECK(!g_marks[21]);
    CHECK(!g_watch); /* the marks alone watch nothing of the GPU's */
    puts("vk_write_marks_scan: ok");
    return 0;
}
