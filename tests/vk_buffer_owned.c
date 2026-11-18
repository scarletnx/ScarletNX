/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A draw that binds a range's owner where it is (gpu/vk/buffer.c buf_get_owned), with no device. Draws write owners
   inside rendering passes too: the page cache's (borrowed) and kept results (VK_OWN_BY_DRAW). When such an owner was
   written in the current submission, buf_get_owned must end the pass and then record a barrier from all work before
   it binds the owner. An owner of another kind, or one written in an earlier submission, takes no barrier, and the
   pass goes on. Each case binds the owner's buffer at its offset. */
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

/* ------------------------------------------------ the model: one GPU range over one guest range, one owner */
#define GVA    0x100000000ull /* the range's GPU address */
#define CPU    0x40000000ull  /* the guest address behind it */
#define LEN    0x2000ull      /* its bytes */
#define SUBMIT 7u             /* the current graphics submission */
#define CMD    ((VkCommandBuffer)(uintptr_t)0x1234)
static struct {
    VkOwnRes res; /* the one owner, over [CPU, CPU + LEN) at RES_OFF in its buffer */
    char log[16]; /* what the function recorded, in order: 'e' a pass end, 'b' a barrier from all work */
    u32 n;
} g_m;
#define RES_OFF 0x300ull
static void note(char c) {
    CHECK(g_m.n + 1 < sizeof g_m.log);
    g_m.log[g_m.n++] = c;
}
u64 gmmu_run(u64 gva, u64 max, u64* cpu) {
    if (gva != GVA) return 0;
    *cpu = CPU;
    return max < LEN ? max : LEN;
}
u32 own_lookup(u64 addr, u64 size, OwnRange* out, u32 max) {
    CHECK(max >= 1);
    if (addr != CPU || size != LEN) return 0;
    out[0] = (OwnRange){0};
    out[0].addr = CPU;
    out[0].size = LEN;
    out[0].res = &g_m.res;
    out[0].res_off = RES_OFF;
    return 1;
}
u64 vk_submit_number(void) { return SUBMIT; }
void vk_render_end_at(const char* file, int line) {
    (void)file, (void)line;
    note('e');
}
VkCommandBuffer vk_cmd(void) { return CMD; }
void vk_all_barrier(VkCommandBuffer cb) {
    CHECK(cb == CMD);
    note('b');
}
u64 vk_compute_adds(void) { return 0; }
u64 gpu_mem_generation(void) { return 0; }
u64 vk_pages_written(void) { return 0; }

/* ------------------------------------------------ what buffer.c reaches otherwise: not used here */
#pragma GCC diagnostic ignored "-Wunused-parameter"
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(2);
}
void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }
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
void gpu_mem_written_cpu(u64 cpu, u64 size) { abort(); }
void gpu_mem_written_shader(u64 gva, u64 size) { abort(); }
u32 gpu_pools(GpuPool* out, u32 cap) { abort(); }
int mem_alias_any(void) { abort(); }
int mem_alias_clear(u64 a, u64 size) {
    (void)a;
    (void)size;
    abort();
}
void mem_alias_peers(u64 a, u64 size, void (*fn)(void*, u64, u64), void* user) { abort(); }
int mem_pool_any(u64 a, u64 size) { abort(); }
int mem_ww_probe(void (*cb)(u8*, u64)) { abort(); }
u64 own_first_trapped(u64 addr, u64 size) { abort(); }
int own_hash(u64 addr, u64 size, u64* h) { abort(); }
int own_traps(void) { abort(); }
void pc_init(PCond* c) { abort(); }
void pc_signal(PCond* c) { abort(); }
int pc_wait(PCond* c, PMutex* m, int64_t timeout_ns) { abort(); }
void pend_add_pages(PendPages* t, u64 first, u64 last, int delta) { abort(); }
int pend_hit(const PendPages* t, u64 gva, u64 size) { abort(); }
u8* plat_alias_mirror(void) { abort(); }
uint64_t plat_ns(void) { abort(); }
int plat_thread(PThreadFn fn, void* arg, size_t stack) { abort(); }
int plat_vm_read(const void* p, void* dst, u64 size) { abort(); }
void plat_ww_cool(u8* p, u64 size, void (*cb)(u8*, u64)) { abort(); }
int plat_ww_hot_in(const u8* p, u64 size) { abort(); }
int plat_ww_scan(u8* p, u64 size, void (*cb)(u8*, u64)) { abort(); }
void plat_yield(void) { abort(); }
void pm_lock(PMutex* m) { abort(); }
void pm_unlock(PMutex* m) { abort(); }
void rec_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, u32 n, const VkBufferCopy* r) { abort(); }
void rec_CmdFillBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkDeviceSize size, u32 data) { abort(); }
void rec_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* d) { abort(); }
void snx_thread_name(const char* name) { abort(); }
VkResult vk_buffer_create(const VkBufferCreateInfo* info, const DevMemWant* allocation, VkBuffer* buffer,
                          DevMem* memory, DevMemInfo* details) {
    abort();
}
VkCommandBuffer vk_cmd_upload(int gpu) { abort(); }
int vk_compute_gpu_src(u64 gva, u64 len, VkBuffer* buf, u64* off) { abort(); }
void vk_compute_mem_need(u64 a, u64 n) { abort(); }
int vk_compute_pending(u64 a, u64 n) { abort(); }
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
u32 vk_present_before(u64 ago_ns, u32 presents) { abort(); }
int vk_query_counts(VkBuffer buf, u64 off, const SamplesCount* counts, u32 n) { abort(); }
u64 vk_range_inside(const VkRange* r, u32 n, u64 start, u64 len) { abort(); }
u32 vk_range_join(VkRange* r, u32 n) { abort(); }
void vk_range_shape(VkRange* r, u32 n, const u64 gran[2], VkRangeShape* out) { abort(); }
int vk_release_pending(u64 gva, u64 len) { abort(); }
int vk_release_read_counts(u64 gva, void* dst, u64 len, SamplesCount* counts, u32 max, u32* n) { abort(); }
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
int vk_uma(void) { abort(); }
void vk_uma_forget_all(void) { abort(); }
void vk_uma_frame(void) { abort(); }
void vk_uma_frame_done(u64 frame) { abort(); }
int vk_uma_get(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off) { abort(); }
int vk_uma_gpu_write(u64 cpu, u64 len, VkBuffer* buf, u64* off) { abort(); }
void vk_uma_gpu_wrote(u64 cpu, u64 len) { abort(); }
void vk_uma_host_barrier(VkCommandBuffer cb, u64 frame) { abort(); }
u64 vk_uma_import_in(u64 cpu, u64 end, u64* hi) { abort(); }
u64 vk_uma_import_part(u64 cpu, u64 len, int* held) { abort(); }
int vk_uma_imports_dear(void) { abort(); }
int vk_uma_pending(u64 gva, u64 len) { abort(); }
int vk_uma_write(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off) { abort(); }
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
void vk_defer_buffer(VkBuffer b, DevMem a) { abort(); }
u64 gmmu_translate(u64 gva) { abort(); }

/* The owner made by `by` (borrowed: the page cache's), its last write in submission `written`, bound for a draw. It
   must give the owner's buffer at its offset, and record what `want` says: "eb" a pass end and then a barrier, "" no
   barrier and no pass end */
static void bind(u8 by, u8 borrowed, u64 written, const char* want) {
    g_m.res = (VkOwnRes){0};
    g_m.res.buf = (VkBuffer)(uintptr_t)0xb0f;
    g_m.res.size = LEN + RES_OFF;
    g_m.res.by = by;
    g_m.res.borrowed = borrowed;
    g_m.res.written = written;
    g_m.n = 0;
    memset(g_m.log, 0, sizeof g_m.log);
    VkBuffer buf = VK_NULL_HANDLE;
    u64 off = 0;
    CHECK(buf_get_owned(0, NULL, GVA, LEN, 0, 1, 16, &buf, &off) == 1);
    CHECK(buf == g_m.res.buf && off == RES_OFF);
    if (strcmp(g_m.log, want)) {
        fprintf(stderr, "FAIL: owner by %u, borrowed %u, written in %llu: recorded \"%s\", want \"%s\"\n", by, borrowed,
                (unsigned long long)written, g_m.log, want);
        exit(1);
    }
}

int main(void) {
    /* a draw's kept result and the page cache's range, written in this submission: inside a pass, with no barrier */
    bind(VK_OWN_BY_DRAW, 0, SUBMIT, "eb");
    bind(VK_OWN_BY_COMPUTE, 1, SUBMIT, "eb");
    /* written in an earlier submission: the submission's own barriers ordered it */
    bind(VK_OWN_BY_DRAW, 0, SUBMIT - 1, "");
    bind(VK_OWN_BY_COMPUTE, 1, SUBMIT - 1, "");
    /* the other makers write outside passes, with a barrier after (vk_own_res_wrote) */
    bind(VK_OWN_BY_COMPUTE, 0, SUBMIT, "");
    bind(VK_OWN_BY_COPY, 0, SUBMIT, "");
    bind(VK_OWN_BY_TARGET, 0, SUBMIT, "");
    puts("vk_buffer_owned: ok");
    return 0;
}
