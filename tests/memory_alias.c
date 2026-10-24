/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Views of guest memory through the alias pool, with the real POSIX platform layer (memfd, userfaultfd write
   tracking): MapMemory's view and its source, code memory's owner and slave, ldr:ro's code view and shared memory are
   the same pages; the pool gives everything back; write tracking keeps working over remapped ranges. Skips (77)
   where the host has no pool */
#include "nx/hle/kernel/kernel_int.h"
#include "nx/hle/hle.h"
#include "nx/gpu/gpu.h"
#include "nx/runtime/clock.h"
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MIB (1ull << 20)
static unsigned checks;

static void test_fail(const char* expr, int line) {
    fprintf(stderr, "memory_alias.c:%d: %s\n", line, expr);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

void logf_(int level, const char* fmt, ...) {
    (void)level;
    (void)fmt;
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    abort();
}
void log_crash_mode(void) {}
void crash_report(void* host_pc, void* fault, int sig) {
    (void)host_pc;
    (void)fault;
    (void)sig;
}
u64 guest_ticks(void) { return 1; }
int kthread_live_count(void) { return 1; }

/* the GPU's side: the bracket must hold around every host change, and the changed ranges are noted */
static int mapping_depth;
static u64 changed_lo = ~0ull, changed_hi;
static void mapping_begin(u64 a, u64 size, int full) {
    (void)a, (void)size, (void)full;
    CHECK(mapping_depth == 0);
    mapping_depth++;
}
static void mapping_end(void) {
    CHECK(mapping_depth == 1);
    mapping_depth--;
}
void (*g_gpu_mapping_begin)(u64, u64, int) = mapping_begin;
void (*g_gpu_mapping_end)(void) = mapping_end;
void gpu_mem_changed(u64 a, u64 size) {
    CHECK(mapping_depth == 0);
    if (a < changed_lo) changed_lo = a;
    if (a + size > changed_hi) changed_hi = a + size;
}
static int changed(u64 a, u64 size) {
    int r = changed_lo <= a && a + size <= changed_hi;
    changed_lo = ~0ull;
    changed_hi = 0;
    return r;
}

/* handle 0x1234 names test_handle; references are counted */
static KObj* test_handle;
KObj* h_get_ref(u32 h, int type) {
    if (h != 0x1234 || !test_handle || test_handle->type != type) return NULL;
    kobj_ref(test_handle);
    return test_handle;
}
void kobj_ref(KObj* o) { o->refs++; }
/* the destroys of the types svc_mem.c makes, as kernel.c's kobj_destroy picks them */
void kobj_unref(KObj* o) {
    if (--o->refs) return;
    if (o->type == KO_SHMEM)
        kshmem_destroy(o);
    else if (o->type == KO_TMEM)
        tmem_destroy(o);
    else if (o->type == KO_CODEMEM)
        kcodemem_destroy(o);
}

static u32 state(u64 a) {
    MemBlock b;
    mem_query(a, &b);
    return b.state;
}
static int count_fds(void) {
    DIR* d = opendir("/proc/self/fd");
    struct dirent* e;
    int n = 0;
    if (!d) return -1;
    while ((e = readdir(d)) != NULL) n += e->d_name[0] != '.';
    closedir(d);
    return n - 1; /* the directory's own */
}
static int count_maps(void) {
    FILE* f = fopen("/proc/self/maps", "r");
    int n = 0, c;
    if (!f) return -1;
    while ((c = fgetc(f)) != EOF) n += c == '\n';
    fclose(f);
    return n;
}

/* written-page tracking, when this kernel has it (6.7) */
static int g_ww;
static u64 g_written_lo, g_written_hi;
static void on_written(u8* p, u64 size) {
    if ((u64)(p - g_mem) < g_written_lo) g_written_lo = (u64)(p - g_mem);
    if ((u64)(p - g_mem) + size > g_written_hi) g_written_hi = (u64)(p - g_mem) + size;
}
/* 1 when a scan of [a, a + size) works and reports page at written, 0 when it works and doesn't, -1 when it fails */
static int scan_reports(u64 a, u64 size, u64 at) {
    g_written_lo = ~0ull;
    g_written_hi = 0;
    if (plat_ww_scan(GP(a), size, on_written)) return -1;
    return g_written_lo <= at && at < g_written_hi;
}
/* every range scans, and a write through it is reported (a scan first takes what remapping marked) */
static void check_tracking(u64 a, u64 size) {
    if (!g_ww) return;
    CHECK(scan_reports(a, size, a) >= 0);
    CHECK(scan_reports(a, size, a) == 0);
    GP(a)[size / 2] ^= 1;
    CHECK(scan_reports(a, size, a + size / 2) == 1);
    CHECK(scan_reports(a, size, a + size / 2) == 0);
}

static u32 info_attr_perm(u64 info, u64 a, u32* perm) {
    CHECK(svc_query(info, a) == 0);
    *perm = gr32(info + 24);
    return gr32(info + 20);
}

/* MapMemory: the view and the source are the same pages; the source is locked while mapped */
static void test_map_memory(u64 info) {
    u64 src = HEAP_BASE + 0x3000, dst = STACK_BASE + 0x10000;
    u32 perm;
    gw32(src, 0xCAFE);
    CHECK(svc_map_memory(dst, src, 0x2000) == 0);
    CHECK(changed(dst, 0x2000));
    CHECK(mem_pool_used() == 0x2000 && state(dst) == MS_STACK);
    CHECK(gr32(dst) == 0xCAFE);
    gw32(dst + 0x1000, 0xBEEF);
    CHECK(gr32(src + 0x1000) == 0xBEEF); /* seen through the source at once: no copy */
    gw32(src + 0x10, 0x5555);            /* (the guest may not, but the host can see it is one page) */
    CHECK(gr32(dst + 0x10) == 0x5555);
    CHECK(info_attr_perm(info, src, &perm) == 1 && perm == PERM_NONE);
    CHECK(svc_map_memory(dst + 0x4000, src, 0x1000) == R_BADMEMSTATE); /* locked */
    CHECK(svc_set_heap_size(0) == R_BADMEMSTATE);
    check_tracking(dst, 0x2000);
    check_tracking(src, 0x2000);
    CHECK(svc_unmap_memory(dst + 0x1000, src, 0x2000) == R_BADMEMSTATE);
    CHECK(svc_unmap_memory(dst, src, 0x2000) == 0);
    CHECK(gr32(src) == 0xCAFE && gr32(src + 0x1000) == 0xBEEF && gr32(src + 0x10) == 0x5555);
    CHECK(state(dst) == MS_FREE && mem_pool_used() == 0);
    CHECK(info_attr_perm(info, src, &perm) == 0 && perm == PERM_RW);
    check_tracking(src, 0x2000);
    CHECK(mem_map(dst, 0x2000, MS_STACK, PERM_RW) == 0 && gr32(dst + 0x1000) == 0); /* fresh pages again */
    CHECK(mem_unmap(dst, 0x2000) == 0);
    /* the alias region takes a view too (normal memory), and a view of that view is the same pages once more */
    CHECK(svc_map_memory(ALIAS_BASE, src, 0x2000) == 0 && state(ALIAS_BASE) == MS_NORMAL);
    CHECK(mem_unmap_physical(ALIAS_BASE, 0x2000) == MEM_BADSTATE); /* a view, not physical memory */
    CHECK(svc_map_memory(dst, ALIAS_BASE, 0x2000) == 0 && mem_pool_used() == 0x2000);
    gw32(dst, 0x7777);
    CHECK(gr32(ALIAS_BASE) == 0x7777 && gr32(src) == 0x7777);
    CHECK(svc_unmap_memory(dst, ALIAS_BASE, 0x2000) == 0 && mem_pool_used() == 0x2000); /* src still holds them */
    CHECK(svc_unmap_memory(ALIAS_BASE, src, 0x2000) == 0 && mem_pool_used() == 0 && gr32(src) == 0x7777);
    /* a view onto memory that isn't free fails and leaves the source as it was */
    CHECK(mem_map(dst, 0x1000, MS_STACK, PERM_RW) == 0);
    CHECK(svc_map_memory(dst, src, 0x2000) == R_BADMEMSTATE);
    CHECK(mem_pool_used() == 0 && info_attr_perm(info, src, &perm) == 0 && perm == PERM_RW && gr32(src) == 0x7777);
    CHECK(mem_unmap(dst, 0x1000) == 0);
    /* two sources side by side get neighbouring pool pages, and their blocks join: unmapping each gives its own back,
       and a later view of heap across both works (a game's thread stacks failed to map in about 1 run in 3) */
    CHECK(svc_map_memory(dst, src, 0x2000) == 0 && svc_map_memory(dst + 0x4000, src + 0x2000, 0x2000) == 0);
    CHECK(mem_pool_used() == 0x4000);
    CHECK(svc_unmap_memory(dst, src, 0x2000) == 0 && mem_pool_used() == 0x2000);
    CHECK(svc_unmap_memory(dst + 0x4000, src + 0x2000, 0x2000) == 0 && mem_pool_used() == 0);
    CHECK(svc_map_memory(dst, src + 0x1000, 0x2000) == 0 && svc_unmap_memory(dst, src + 0x1000, 0x2000) == 0);
    CHECK(mem_pool_used() == 0 && gr32(src) == 0x7777);
}

/* code memory: the owner's writes are in the slave at once */
static void test_code_memory(void) {
    u64 src = HEAP_BASE + 0x10000, owner = 0x40000000ull, slave = 0x50000000ull;
    KObj* o = NULL;
    MemBlock b;
    gw32(src, 0x11);
    CHECK(svc_codemem_new(src, 0x4000, &o) == 0 && o);
    CHECK(mem_pool_used() == 0x4000);
    CHECK(svc_codemem_control(o, 1, slave, 0x4000, PERM_RX) == 0);
    CHECK(svc_codemem_control(o, 0, owner, 0x4000, PERM_RW) == 0);
    CHECK(gr32(slave) == 0x11 && gr32(owner) == 0x11);
    gw32(owner + 0x2000, 0xD503201F); /* a JIT writes an instruction after the slave is mapped */
    CHECK(gr32(slave + 0x2000) == 0xD503201F);
    check_tracking(owner, 0x4000);
    check_tracking(slave, 0x4000);
    CHECK(svc_codemem_control(o, 2, owner, 0x4000, 0) == 0 && state(owner) == MS_FREE);
    CHECK(gr32(slave + 0x2000) == 0xD503201F);
    CHECK(svc_codemem_control(o, 0, owner + 0x10000, 0x4000, PERM_RW) == 0);
    CHECK(gr32(owner + 0x10000 + 0x2000) == 0xD503201F);
    gw32(owner + 0x10000 + 4, 0x22);
    CHECK(gr32(slave + 4) == 0x22);
    CHECK(svc_codemem_control(o, 0, owner, 0x4000, PERM_RW) == R_BADSTATE);
    kcodemem_destroy(o);
    CHECK(mem_pool_used() == 0 && state(owner + 0x10000) == MS_FREE && state(slave) == MS_FREE);
    mem_query(src, &b);
    CHECK(b.perm == PERM_RW && b.attr == 0 && b.pool == 0);
    CHECK(gr32(src + 0x2000) == 0xD503201F && gr32(src + 4) == 0x22);
    check_tracking(src, 0x4000);
}

/* ldr:ro's code view of an image and its BSS */
static void test_code_view(void) {
    MemCodeMap m = {0, HEAP_BASE + 0x20000, 0x3000, HEAP_BASE + 0x40000, 0x2000};
    MemBlock b;
    gw32(m.source, 0x4E524F30);
    CHECK(mem_map_code(&m) == 0 && m.base);
    CHECK(mem_pool_used() == 0x5000 && gr32(m.base) == 0x4E524F30);
    CHECK(mem_set_state(m.base, 0x1000, MS_ALIASCODE, PERM_RX) == 0); /* what ldr does to the text */
    gw32(m.base + 0x2000, 1);
    gw32(m.base + 0x3000 + 0x1000, 2); /* the BSS */
    CHECK(gr32(m.source + 0x2000) == 1 && gr32(m.bss + 0x1000) == 2);
    mem_query(m.source, &b);
    CHECK(b.perm == PERM_NONE && b.attr == 1);
    check_tracking(m.base, 0x5000);
    CHECK(mem_unmap_code(&m) == 0);
    CHECK(mem_pool_used() == 0 && state(m.base) == MS_FREE);
    CHECK(gr32(m.source + 0x2000) == 1 && gr32(m.bss + 0x1000) == 2 && gr32(m.source) == 0x4E524F30);
    mem_query(m.bss, &b);
    CHECK(b.perm == PERM_RW && b.attr == 0 && b.pool == 0);
    check_tracking(m.source, 0x3000);
}

/* shared memory: the service's pointer and every guest mapping are the same pages */
static void test_shared_memory(void) {
    KShmem* s = kshmem_new(0x2000, "test");
    u64 a = ALIAS_BASE - 0x100000, b = ALIAS_BASE - 0x200000;
    u8* host;
    CHECK(s->pool && mem_pool_used() == 0x2000);
    s->o.refs = 1; /* the service's */
    test_handle = &s->o;
    host = kshmem_ptr(s);
    host[0] = 7;
    CHECK(svc_map_shm(0x1234, a, 0x2000, PERM_RW) == 0 && changed(a, 0x2000));
    CHECK(GP(a)[0] == 7 && state(a) == MS_SHARED && kshmem_ptr(s) == host);
    CHECK(s->o.refs == 2); /* the mapping's */
    GP(a)[0x1001] = 9;
    CHECK(host[0x1001] == 9);
    host[2] = 3;
    CHECK(GP(a)[2] == 3);
    /* a second mapping at once, a larger one past the object's pages */
    CHECK(svc_map_shm(0x1234, b, 0x3000, PERM_R) == 0 && state(b + 0x2000) == MS_SHARED && s->o.refs == 3);
    CHECK(GP(b)[0x1001] == 9 && GP(b)[2] == 3);
    GP(b)[5] = 5;
    CHECK(GP(a)[5] == 5 && host[5] == 5);
    check_tracking(a, 0x2000);
    check_tracking(b, 0x3000);
    CHECK(svc_unmap_shm(0x1234, a + 0x1000, 0x2000) == R_BADMEMSTATE);
    CHECK(svc_unmap_shm(0x1234, a, 0x2000) == 0 && state(a) == MS_FREE && s->o.refs == 2);
    CHECK(svc_unmap_shm(0x1234, a, 0x2000) == R_BADMEMSTATE);
    CHECK(GP(b)[5] == 5);
    CHECK(svc_unmap_shm(0x1234, b, 0x3000) == 0 && state(b + 0x2000) == MS_FREE && s->o.refs == 1);
    CHECK(svc_map_shm(0x1234, b, 0x2000, PERM_RW) == 0 && GP(b)[0x1001] == 9 && GP(b)[5] == 5);
    CHECK(svc_unmap_shm(0x1234, b, 0x2000) == 0);
    test_handle = NULL;
    kobj_unref(&s->o);
    CHECK(mem_pool_used() == 0);
}

/* a full pool: the calls fail with OutOfMemory and change nothing; shared memory falls back to a copy */
static void test_pool_full(u64 info) {
    u64 src = HEAP_BASE + 0x3000, dst = STACK_BASE + 0x10000, keep = g_pool_size;
    KObj* o = NULL;
    KShmem* s;
    u32 perm;
    g_pool_size = 0x1000;
    CHECK(svc_map_memory(dst, src, 0x2000) == R_OUTOFMEM);
    CHECK(state(dst) == MS_FREE && info_attr_perm(info, src, &perm) == 0 && perm == PERM_RW);
    CHECK(svc_codemem_new(src, 0x2000, &o) == R_OUTOFMEM && !o);
    CHECK(info_attr_perm(info, src, &perm) == 0 && perm == PERM_RW);
    s = kshmem_new(0x2000, "copy");
    CHECK(s && !s->pool && kshmem_ptr(s) == s->host);
    kshmem_destroy(&s->o);
    CHECK(svc_map_memory(dst, src, 0x1000) == 0 && mem_pool_used() == 0x1000); /* what fits */
    CHECK(svc_unmap_memory(dst, src, 0x1000) == 0 && mem_pool_used() == 0);
    g_pool_size = keep;
}

/* many cycles leave no descriptor, mapping or pool page behind, and tracking still works */
static void test_cycles(void) {
    u64 src = HEAP_BASE + 0x80000, dst = STACK_BASE + 0x40000;
    int fds = count_fds(), maps, i;
    CHECK(svc_map_memory(dst, src, 0x8000) == 0 && svc_unmap_memory(dst, src, 0x8000) == 0); /* settle the VMAs */
    maps = count_maps();
    for (i = 0; i < 1000; i++) {
        KShmem* s;
        gw32(src, (u32)i);
        CHECK(svc_map_memory(dst + (u64)(i & 7) * 0x10000, src, 0x8000) == 0);
        CHECK(gr32(dst + (u64)(i & 7) * 0x10000) == (u32)i);
        CHECK(svc_unmap_memory(dst + (u64)(i & 7) * 0x10000, src, 0x8000) == 0);
        s = kshmem_new(0x1000, "cycle");
        kshmem_destroy(&s->o);
    }
    CHECK(mem_pool_used() == 0 && count_fds() == fds);
    CHECK(count_maps() <= maps + 2);
    check_tracking(src, 0x8000);
    check_tracking(HEAP_BASE, 0x3000); /* the heap's own pages beside it */
}


/* GPU-owned physical pages retain their normal guest state after moving into the alias pool. */
static void test_physical_pool(void)
{
	u64 a = ALIAS_BASE + 0x1000000;
	MemPoolRun runs[4];
	u32 n;
	MemBlock b;
	u64 used = mem_pool_used();

	CHECK(mem_map_physical(a, 0x4000, 1ull << 30) == 0);
	CHECK(mem_own_prepare(a, 0x3000, runs, 4, &n) == 0 && n == 1);
	CHECK(mem_pool_used() == used + 0x3000);
	CHECK(mem_unmap_physical(a + 0x1000, 0x1000) == 0);
	CHECK(mem_pool_used() == used + 0x2000);
	mem_query(a, &b);
	CHECK(b.state == MS_NORMAL && b.pool);
	mem_query(a + 0x2000, &b);
	CHECK(b.state == MS_NORMAL && b.pool);
	CHECK(mem_unmap_physical(a, 0x4000) == 0);
	CHECK(mem_pool_used() == used && g_phys_mapped == 0);
	CHECK(mem_map_physical(a, 0x4000, 1ull << 30) == 0);
	CHECK(gr32(a) == 0 && gr32(a + 0x2000) == 0);
	CHECK(mem_unmap_physical(a, 0x4000) == 0);
}

int main(void) {
    hle_init();
    u64 info = STACK_BASE, off;
    u8* p;
    if (sysconf(_SC_PAGESIZE) != 4096) {
        printf("skipped: this host's pages are %ld bytes, and guest memory needs 4 KiB ones\n", sysconf(_SC_PAGESIZE));
        return 77;
    }
    mem_init();
    p = mem_pool_host(0x1000, &off);
    if (!p) {
        printf("skipped: no alias pool on this host (views are copies)\n");
        return 77;
    }
    mem_pool_host_free(p, off, 0x1000);
    g_ww = plat_ww_scan(g_mem, 4096, on_written) == 0;
    if (!g_ww) printf("written-page tracking unavailable here: not checking it\n");
    g_code_memory = 55 * MIB;
    g_main_stack_size = MIB;
    CHECK(svc_set_heap_size(2 * MIB) == 0);
    CHECK(mem_map(info, 0x1000, MS_STACK, PERM_RW) == 0);
    check_tracking(HEAP_BASE, 2 * MIB);
    test_map_memory(info);
    test_code_memory();
    test_code_view();
    test_shared_memory();
    test_pool_full(info);
    test_cycles();
    test_physical_pool();
    CHECK(mapping_depth == 0);
    if (g_ww) CHECK(plat_ww_scan(g_mem, AS_SIZE, on_written) == 0); /* tracking never went off */
    printf("memory alias: %u checks passed%s\n", checks, g_ww ? "" : " (no write tracking)");
    return 0;
}
