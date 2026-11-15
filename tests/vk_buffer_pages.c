/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GPU pages of a range as the buffer cache reads them (gpu/vk/buffer.c), over a model of the GMMU, with no device:
   gpu_pages, range_map (RangeMap), out_map and range_pages walk the table one run at a time with gmmu_run. Each must
   give the pages and the run flag that a walk with gmmu_translate for each page gave: on a layout that is one run, one
   with a hole that a run crosses, one with runs that jump up and down and pages mapped twice, one with guest pages
   freed under a mapping, and random layouts. They ask gmmu_run once for each run and each unmapped page, and never
   ask gmmu_translate. out_spans, which takes the range's run flag from range_map, must give the byte pairs that a
   walk of each byte gives. */
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

/* ------------------------------------------------ the GMMU */
#define GVA    0x100000000ull /* the first GPU page of the model */
#define NPAGES 64u            /* the GPU pages the model maps */
#define CPAGES 0x800u         /* the guest pages there are: a page above them is not mapped in guest memory */
static struct {
    u64 pte[NPAGES];      /* the guest page behind each GPU page, 0: not mapped */
    u8 freed[CPAGES];     /* a guest page freed after it was mapped: gmmu_translate reads it as unmapped */
    u32 runs, translates; /* calls of gmmu_run and gmmu_translate */
} g_m;
static int guest_page_ok(u64 cpage) { return cpage && cpage < CPAGES && !g_m.freed[cpage]; }
static u64 gpu_page_of(u64 gva) { return gva >= GVA && gva - GVA < (u64)NPAGES << 12 ? (gva - GVA) >> 12 : NPAGES; }
/* as nvdrv.c's: 0 when gva's page is not mapped or its guest page is freed */
u64 gmmu_translate(u64 gva) {
    u64 p = gpu_page_of(gva);
    g_m.translates++;
    if (p == NPAGES || !guest_page_ok(g_m.pte[p])) return 0;
    return g_m.pte[p] << 12 | (gva & 4095);
}
/* as nvdrv.c's: the bytes from gva on, at most max, whose pages follow on in guest memory, and stop before a page
   that is freed. 0 when gva's page is not mapped or its guest page is freed */
u64 gmmu_run(u64 gva, u64 max, u64* cpu) {
    u64 p = gpu_page_of(gva);
    g_m.runs++;
    if (!max || p == NPAGES || !guest_page_ok(g_m.pte[p])) return 0;
    *cpu = g_m.pte[p] << 12 | (gva & 4095);
    u64 n = 4096 - (gva & 4095);
    while (n < max && p + 1 < NPAGES && g_m.pte[p + 1] == g_m.pte[p] + 1 && guest_page_ok(g_m.pte[p + 1])) {
        p++;
        n += 4096;
    }
    return n < max ? n : max;
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
u64 g_as_size;
_Atomic(int) g_cs_any;
_Atomic(int) g_gfx_any;
_Atomic(int) g_gfx_img_any;
int g_gpu_releases;
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
int mem_alias_any(void) { abort(); }
void mem_alias_peers(u64 a, u64 size, void (*fn)(void*, u64, u64), void* user) { abort(); }
int mem_pool_any(u64 a, u64 size) { abort(); }
int mem_ww_probe(void (*cb)(u8*, u64)) { abort(); }
u64 own_first_trapped(u64 addr, u64 size) { abort(); }
int own_hash(u64 addr, u64 size, u64* h) { abort(); }
u32 own_lookup(u64 addr, u64 size, OwnRange* out, u32 max) { abort(); }
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
void vk_all_barrier(VkCommandBuffer cb) { abort(); }
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
u64 vk_uma_out_adds(void) { abort(); }
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

/* ------------------------------------------------ the walks before this change: gmmu_translate for each page */
static int ref_pages(u64 base, u64 n, u64* map) {
    int run = 1;
    for (u64 i = 0; i < n; i++) {
        u64 cpu = gmmu_translate(base + i * 4096);
        map[i] = cpu ? cpu >> 12 : ~0ull;
        run &= cpu && map[i] == map[0] + i;
    }
    return run;
}
static void ref_range_pages(u64 a, u64 an, RangePages* q) {
    q->n = 0;
    q->lo = ~0ull;
    q->hi = 0;
    q->a = a;
    q->an = an;
    q->run = 1;
    for (u64 ap = a >> 12, ae = (a + an - 1) >> 12; ap <= ae; ap++) {
        u64 ac = gmmu_translate(ap << 12);
        if (!ac) {
            q->run = 0;
            continue;
        }
        u64 page = ac >> 12;
        if (q->n && page != q->hi + 1) q->run = 0;
        if (q->n < 16) q->page[q->n] = page;
        q->n++;
        if (page < q->lo) q->lo = page;
        if (page > q->hi) q->hi = page;
    }
}
/* the gmmu_run calls that a walk of map (n pages) takes: one for each unmapped page, and one for each run */
static u32 runs_of(const u64* map, u64 n) {
    u32 k = 0;
    for (u64 i = 0; i < n; i++) k += map[i] == ~0ull || !i || map[i - 1] == ~0ull || map[i - 1] + 1 != map[i];
    return k;
}

/* ------------------------------------------------ the checks */
static u32 g_seed = 0x6a09e667u;
static u32 rnd(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}
static u32 g_cases;
/* [gva, gva + len): its pages through gpu_pages, range_map, range_pages and out_map, against the walks before */
static void check_range(u64 gva, u64 len) {
    u64 base = gva & ~4095ull, n = (gva + len - base + 4095) >> 12;
    u64 want[NPAGES + 32], got[NPAGES + 32];
    g_cases++;
    CHECK(n <= NPAGES + 32);
    int run = ref_pages(base, n, want);
    u32 runs = runs_of(want, n);
    /* gpu_pages */
    g_m.runs = g_m.translates = 0;
    memset(got, 0xA5, sizeof got);
    CHECK(gpu_pages(base, n, got) == run);
    CHECK(!memcmp(got, want, n * sizeof *got) && got[n] == 0xA5A5A5A5A5A5A5A5ull);
    CHECK(g_m.runs == runs && !g_m.translates);
    /* range_map: made once, in local when 16 pages do */
    RangeMap rm = {0};
    g_m.runs = 0;
    CHECK(range_map(&rm, gva, len) && rm.run == run && !memcmp(rm.map, want, n * sizeof *want));
    CHECK((rm.map == rm.local) == (n <= 16) && g_m.runs == runs);
    CHECK(range_map(&rm, gva, len) && g_m.runs == runs);
    range_map_free(&rm);
    /* range_pages */
    RangePages q, r;
    memset(&q, 0x5A, sizeof q);
    memset(&r, 0x5A, sizeof r);
    ref_range_pages(gva, len, &r);
    g_m.runs = g_m.translates = 0;
    range_pages(gva, len, &q);
    CHECK(q.n == r.n && q.lo == r.lo && q.hi == r.hi && q.a == r.a && q.an == r.an && q.run == r.run);
    CHECK(!memcmp(q.page, r.page, (r.n < 16 ? r.n : 16) * sizeof *q.page));
    CHECK(g_m.runs == runs && !g_m.translates);
    /* out_map, for a result over the same range */
    GfxOut o = {0};
    u64 sorted[NPAGES + 32];
    o.addr = gva;
    o.size = (u32)len;
    o.npages = (u32)n;
    o.pages = malloc(n * sizeof *o.pages);
    o.vmap = malloc(n * sizeof *o.vmap);
    CHECK(o.pages && o.vmap);
    g_m.runs = g_m.translates = 0;
    out_map(&o, gva);
    memcpy(sorted, want, n * sizeof *want);
    qsort(sorted, n, sizeof *sorted, page_cmp);
    CHECK(!memcmp(o.vmap, want, n * sizeof *want) && !memcmp(o.pages, sorted, n * sizeof *sorted));
    CHECK(o.cpu == (run ? want[0] << 12 | (gva & 4095) : ~0ull));
    CHECK(g_m.runs == runs && !g_m.translates);
    free(o.pages);
    free(o.vmap);
}
/* every range from each start (on a page, one byte in, in the middle, one byte before the next page) of each length,
   through the model's pages and two past them */
static void check_ranges(void) {
    static const u64 offs[] = {0, 1, 0x800, 0xfff};
    static const u64 lens[] = {1,      2,       0xfff,   0x1000,  0x1001,  0x2fff,
                               0x3005, 0x10000, 0x10001, 0x11000, 0x20000, 0x40000};
    for (u64 sp = 0; sp < NPAGES + 2; sp++)
        for (u32 i = 0; i < sizeof offs / sizeof *offs; i++)
            for (u32 k = 0; k < sizeof lens / sizeof *lens; k++) check_range(GVA + (sp << 12) + offs[i], lens[k]);
}

/* the byte pairs (at, from) that out_spans gives, one for each byte, and those of a walk of each byte */
static struct {
    u64 v[1u << 17];
    u32 n;
} g_pairs, g_want;
static void span_pairs(void* user, u64 at, u64 from, u64 n) {
    (void)user;
    for (u64 i = 0; i < n; i++) {
        CHECK(g_pairs.n < sizeof g_pairs.v / sizeof *g_pairs.v);
        g_pairs.v[g_pairs.n++] = (at + i) << 32 | (from + i);
    }
}
/* a byte of [gva, gva + len) is a byte of o when it is the same guest memory, through any of o's pages; an unmapped
   one only through the same GPU address, where o's page is unmapped too */
static void want_pairs(const GfxOut* o, u64 gva, u64 len) {
    u64 obase = o->addr & ~4095ull;
    g_want.n = 0;
    for (u64 at = 0; at < len; at++) {
        u64 g = gva + at, c = gmmu_translate(g);
        for (u64 j = 0; j < o->npages; j++) {
            u64 og = obase + j * 4096 + (g & 4095);
            if (og < o->addr || og - o->addr >= o->size) continue;
            if (c ? o->vmap[j] != ~0ull && (o->vmap[j] << 12 | (g & 4095)) == c : og == g && o->vmap[j] == ~0ull) {
                CHECK(g_want.n < sizeof g_want.v / sizeof *g_want.v);
                g_want.v[g_want.n++] = at << 32 | (og - o->addr);
            }
        }
    }
}
static void check_spans(u32 count) {
    for (u32 c = 0; c < count; c++) {
        u64 gva = GVA + rnd() % (60u << 12), len = 1 + rnd() % (5u << 12);
        u64 oaddr = rnd() % 2 ? gva - (gva - GVA < 0x1800 ? 0 : 0x1800) + rnd() % 0x3000 : GVA + rnd() % (60u << 12);
        GfxOut o = {0};
        o.addr = oaddr;
        o.size = 1 + rnd() % (5u << 12);
        o.npages = (u32)((o.addr + o.size - (o.addr & ~4095ull) + 4095) >> 12);
        o.pages = malloc(o.npages * sizeof *o.pages);
        o.vmap = malloc(o.npages * sizeof *o.vmap);
        CHECK(o.pages && o.vmap);
        out_map(&o, o.addr);
        RangeMap rm = {0};
        CHECK(range_map(&rm, gva, len));
        g_pairs.n = 0;
        out_spans(&o, gva, len, rm.map, rm.run, span_pairs, NULL);
        want_pairs(&o, gva, len);
        qsort(g_pairs.v, g_pairs.n, sizeof *g_pairs.v, page_cmp);
        qsort(g_want.v, g_want.n, sizeof *g_want.v, page_cmp);
        CHECK(g_pairs.n == g_want.n && !memcmp(g_pairs.v, g_want.v, g_want.n * sizeof *g_want.v));
        range_map_free(&rm);
        free(o.pages);
        free(o.vmap);
        g_cases++;
    }
}

int main(void) {
    /* one run */
    for (u32 i = 0; i < NPAGES; i++) g_m.pte[i] = 0x100 + i;
    check_ranges();
    check_spans(300);
    /* a hole of three pages, which the run on both sides crosses: the pages after it follow on from those before */
    g_m.pte[10] = g_m.pte[11] = g_m.pte[12] = 0;
    g_m.pte[40] = 0;
    check_ranges();
    check_spans(300);
    /* runs that jump up and down, and pages mapped two times: 50 to 53 show the guest pages of 0 to 3 */
    for (u32 i = 0; i < NPAGES; i++) g_m.pte[i] = i < 20 ? 0x100 + i : i < 40 ? 0x300 + i : 0x200 + i;
    for (u32 i = 50; i < 54; i++) g_m.pte[i] = 0x100 + i - 50;
    g_m.pte[30] = 0x300 + 30 + 1; /* a page that skips one */
    check_ranges();
    check_spans(300);
    /* one run, with guest pages freed under it */
    for (u32 i = 0; i < NPAGES; i++) g_m.pte[i] = 0x100 + i;
    g_m.freed[0x100 + 30] = g_m.freed[0x100 + 31] = g_m.freed[0x100 + 63] = g_m.freed[0x100] = 1;
    check_ranges();
    check_spans(300);
    memset(g_m.freed, 0, sizeof g_m.freed);
    /* random layouts: a page follows the last, or is unmapped, or shows a guest page of a small pool (shown again by
       other pages), some of which are freed */
    for (u32 l = 0; l < 24; l++) {
        for (u32 i = 0; i < NPAGES; i++) {
            u32 r = rnd() % 8;
            g_m.pte[i] = r == 0 ? 0 : r < 6 && i && g_m.pte[i - 1] ? g_m.pte[i - 1] + 1 : 0x100 + rnd() % 0x40;
        }
        memset(g_m.freed, 0, sizeof g_m.freed);
        for (u32 i = 0; i < 4; i++) g_m.freed[0x100 + rnd() % 0x48] = 1;
        check_ranges();
        check_spans(200);
    }
    printf("vk_buffer_pages: %u cases passed\n", g_cases);
    return 0;
}
