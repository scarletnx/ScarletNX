/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise the production memory SVCs and block list without game files. The POSIX backing is sparse, with
   injectable commit failures. Thread/handle/GPU services are stubs, not the code under test. */
#include "nx/memory/own.h"
#include "nx/hle/kernel/kernel_int.h"
#include "nx/hle/hle.h"
#include "nx/runtime/clock.h"
#include "nx/gpu/gpu.h"
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <unistd.h>

#define MIB (1ull << 20)
static unsigned checks;
static int fail_commit_after, commit_calls, decommit_calls;
static int mapping_depth, mapping_begins, mapping_ends;

static void test_fail(const char* expr, int line) {
    fprintf(stderr, "memory_accounting.c:%d: %s\n", line, expr);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

void pm_init(PMutex* m) { pthread_mutex_init(&m->l, NULL); }
void pm_lock(PMutex* m) { pthread_mutex_lock(&m->l); }
void pm_unlock(PMutex* m) { pthread_mutex_unlock(&m->l); }
void pm_lock_announce(PMutex* m, const char* name) { (void)m, (void)name; }
/* the host page mem.c works in: the suite runs with this host's, then as with 16 and 64 KiB ones (main) */
static u64 host_page;
u64 plat_vm_page(void) { return host_page; }
/* aligned to host_page, a page more reserved for that (the unaligned head and tail stay inaccessible) */
static u8* reservation;
static u64 reservation_size;
u8* plat_vm_reserve(u64 size) {
    void* p = mmap(NULL, size + host_page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    reservation = (u8*)p;
    reservation_size = size + host_page;
    return (u8*)(((uintptr_t)p + host_page - 1) & ~(uintptr_t)(host_page - 1));
}
int plat_vm_commit(u8* p, u64 size) {
    if (mapping_begins) CHECK(mapping_depth == 1);
    commit_calls++;
    if (fail_commit_after && !--fail_commit_after) return -1;
    return mprotect(p, size, PROT_READ | PROT_WRITE);
}
void plat_vm_decommit(u8* p, u64 size) {
    if (mapping_begins) CHECK(mapping_depth == 1);
    decommit_calls++;
#if defined(__linux__)
    if (madvise(p, size, MADV_DONTNEED) || mprotect(p, size, PROT_NONE)) abort();
#else
    /* as plat_posix.c: only Linux's MADV_DONTNEED drops the contents (Darwin's and FreeBSD's keep them) */
    if (mmap(p, size, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED) abort();
#endif
}
/* no alias pool here: views are copies (memory_alias tests the pool) */
int plat_alias_init(u64 size) {
    (void)size;
    return -1;
}
int plat_alias_map(u8* p, u64 off, u64 size) {
    (void)p, (void)off, (void)size;
    return -1;
}
int plat_alias_unmap(u8* p, u64 size) {
    (void)p, (void)size;
    return -1;
}
int plat_alias_write(u64 off, const void* src, u64 size) {
    (void)off, (void)src, (void)size;
    return -1;
}
int plat_alias_read(u64 off, void* dst, u64 size) {
    (void)off, (void)dst, (void)size;
    return -1;
}
void plat_alias_free(u64 off, u64 size) { (void)off, (void)size; }
u8* plat_alias_host(u64 off, u64 size) {
    (void)off, (void)size;
    return NULL;
}
void plat_alias_host_free(u8* p, u64 size) { (void)p, (void)size; }
/* memory/own.c and the freeze its pages need: nothing is GPU-owned here */
void own_acquire(u64 addr, u64 size) { (void)addr, (void)size; }
void own_drop(u64 addr, u64 size) { (void)addr, (void)size; }
u32 own_lookup(u64 addr, u64 size, OwnRange* out, u32 max) { /* nothing is the GPU's here */
    (void)addr, (void)size, (void)out, (void)max;
    return 0;
}
void plat_freeze(u8* p, u64 size) { (void)p, (void)size; }
void plat_thaw(void) {}
void gpu_mem_changed(u64 a, u64 size) {
    (void)a;
    (void)size;
}
static void mapping_begin(u64 a, u64 size, int full) {
    CHECK(a == 0 && size == 0 && full == 1);
    CHECK(mapping_depth == 0);
    mapping_depth++;
    mapping_begins++;
    /* The renderer lock must come before mem.c's lock: Vulkan may query guest mappings while landing. */
    MemBlock b;
    mem_query(HEAP_BASE, &b);
}
static void mapping_end(void) {
    CHECK(mapping_depth == 1);
    mapping_depth--;
    mapping_ends++;
}
void (*g_gpu_mapping_begin)(u64, u64, int);
void (*g_gpu_mapping_end)(void);
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
u64 plat_ns(void) { return 1; }
u64 guest_ticks(void) { return 1; }
int kthread_live_count(void) { return 1; }
static KObj* test_handle; /* what handle 0x1234 names, for the shared memory test */
KObj* h_get_ref(u32 h, int type) {
    return h == 0x1234 && test_handle && test_handle->type == type ? test_handle : NULL;
}
void kobj_ref(KObj* o) { (void)o; }
void kobj_unref(KObj* o) { (void)o; }

static u64 info(u32 id) {
    u64 out = ~0ull;
    CHECK(svc_get_info(id, H_CUR_PROC, 0, &out) == 0);
    return out;
}
static u32 state(u64 a) {
    MemBlock b;
    mem_query(a, &b);
    return b.state;
}
static void base_process(void) {
    CHECK(g_heap_size == 0 && g_phys_mapped == 0);
    g_settings.memory_gb = 0;
    g_code_memory = 55 * MIB;
    g_main_stack_size = MIB;
    g_hle.kernel.sysres_size = 0;
    g_heap_max = 0;
    fail_commit_after = 0;
}
static void test_heap(void) {
    base_process();
    CHECK(mem_total() == 3285 * MIB);
    CHECK(used_memory() == 56 * MIB);
    CHECK(info(6) == 3285 * MIB && info(7) == 56 * MIB);
    CHECK(info(21) == info(6) && info(22) == info(7));
    CHECK(svc_set_heap_size(3200 * MIB) == 0); /* the reported startup failure */
    CHECK(g_heap_size == 3200 * MIB && used_memory() == 3256 * MIB);
    CHECK(state(HEAP_BASE) == MS_NORMAL);
    CHECK(svc_set_heap_size(3228 * MIB) == 0);
    int calls = commit_calls;
    CHECK(svc_set_heap_size(3230 * MIB) == R_LIMIT);
    CHECK(commit_calls == calls && g_heap_size == 3228 * MIB);
    CHECK(svc_set_heap_size(3228 * MIB) == 0);
    CHECK(commit_calls == calls);
    CHECK(svc_set_heap_size(0) == 0 && g_heap_size == 0);
    CHECK(state(HEAP_BASE) == MS_FREE);
    CHECK(svc_set_heap_size(3200 * MIB + 1) == R_BADSIZE);
    CHECK(svc_set_heap_size(8ull << 30) == R_BADSIZE);
    CHECK(svc_set_heap_size(~0ull) == R_BADSIZE);
    g_heap_max = 3000 * MIB;
    CHECK(svc_set_heap_size(3200 * MIB) == R_OUTOFMEM);
    g_heap_max = 0;
    CHECK(svc_set_heap_size(2 * MIB) == 0);
    fail_commit_after = 1;
    CHECK(svc_set_heap_size(4 * MIB) == R_OUTOFMEM);
    CHECK(g_heap_size == 2 * MIB && state(HEAP_BASE + 2 * MIB) == MS_FREE);
    CHECK(svc_set_heap_size(0) == 0);
    for (int gb = 4; gb <= 8; gb += 2) {
        g_settings.memory_gb = gb;
        CHECK(mem_total() == (gb == 4 ? 3285 : gb == 6 ? 4916 : 6964) * MIB);
        CHECK(svc_set_heap_size(3200 * MIB) == 0);
        CHECK(svc_set_heap_size(0) == 0);
    }
}
static void test_system_resource(void) {
    base_process();
    CHECK(svc_map_physical(ALIAS_BASE, 0x1000) == R_BADSTATE);
    CHECK(svc_unmap_physical(ALIAS_BASE, 0x1000) == R_BADSTATE);
    g_hle.kernel.sysres_size = 64 * MIB;
    CHECK(info(16) == 64 * MIB);
    CHECK(info(6) == 3285 * MIB && info(21) == 3221 * MIB);
    CHECK(info(7) == 120 * MIB && info(22) == 56 * MIB);
    CHECK(svc_set_heap_size(3200 * MIB) == R_LIMIT);
    CHECK(svc_set_heap_size(3164 * MIB) == 0);
    CHECK(svc_map_physical(ALIAS_BASE, 2 * MIB) == R_LIMIT);
    CHECK(g_phys_mapped == 0 && state(ALIAS_BASE) == MS_FREE);
    CHECK(svc_set_heap_size(0) == 0);
    CHECK(svc_map_physical(ALIAS_BASE, 2 * MIB) == 0);
    CHECK(g_phys_mapped == 2 * MIB);
    CHECK(svc_map_physical(ALIAS_BASE + MIB, 2 * MIB) == 0);
    CHECK(g_phys_mapped == 3 * MIB); /* overlap is charged once */
    int calls = commit_calls;
    CHECK(svc_map_physical(ALIAS_BASE, 3 * MIB) == 0 && commit_calls == calls);
    CHECK(svc_set_heap_size(3164 * MIB) == R_LIMIT);     /* the opposite allocation order */
    CHECK(svc_unmap_physical(ALIAS_BASE, 4 * MIB) == 0); /* includes a free tail */
    CHECK(g_phys_mapped == 0);
    CHECK(svc_unmap_physical(ALIAS_BASE, 4 * MIB) == 0);
    CHECK(svc_map_physical(ALIAS_BASE + 1, 0x1000) == R_BADADDR);
    CHECK(svc_map_physical(ALIAS_BASE, 0) == R_BADSIZE);
    CHECK(svc_map_physical(ALIAS_BASE, 1) == R_BADSIZE);
    CHECK(svc_map_physical(0xfffffffffffff000ull, 0x2000) == R_BADREGION);
    CHECK(svc_map_physical(HEAP_BASE, 0x1000) == R_BADREGION);
    CHECK(svc_map_physical(ALIAS_BASE + ALIAS_SIZE - 0x1000, 0x2000) == R_BADREGION);
}
static void test_rollback(void) {
    base_process();
    g_hle.kernel.sysres_size = 2 * MIB;
    u64 a = ALIAS_BASE;
    CHECK(svc_map_physical(a + 0x1000, 0x1000) == 0);
    gw32(a + 0x1000, 0x12345678);
    int decommits = decommit_calls;
    fail_commit_after = 2; /* first hole succeeds, second fails */
    CHECK(svc_map_physical(a, 0x3000) == R_OUTOFMEM);
    /* the first hole given back: decommitted, or zeroed where it shares a host page with a + 0x1000 */
    CHECK(decommit_calls == decommits + (host_page == 0x1000));
    CHECK(g_phys_mapped == 0x1000);
    CHECK(state(a) == MS_FREE && state(a + 0x2000) == MS_FREE);
    CHECK(state(a + 0x1000) == MS_NORMAL && gr32(a + 0x1000) == 0x12345678);
    CHECK(svc_map_physical(a, 0x3000) == 0);
    CHECK(g_phys_mapped == 0x3000 && gr32(a + 0x1000) == 0x12345678);
    CHECK(gr32(a) == 0 && gr32(a + 0x2000) == 0);
    CHECK(svc_unmap_physical(a, 0x3000) == 0);
    CHECK(g_phys_mapped == 0);
    /* A failed unmap must not remove either normal pages or an unrelated shared mapping. */
    CHECK(svc_map_physical(a, 0x1000) == 0);
    CHECK(mem_map(a + 0x1000, 0x1000, MS_SHARED, PERM_RW) == 0);
    CHECK(svc_unmap_physical(a, 0x2000) == R_BADMEMSTATE);
    CHECK(g_phys_mapped == 0x1000 && state(a) == MS_NORMAL && state(a + 0x1000) == MS_SHARED);
    CHECK(mem_unmap(a + 0x1000, 0x1000) == 0);
    CHECK(mem_set_attr(a, 0x1000, 1, 1) == 0);
    CHECK(svc_unmap_physical(a, 0x1000) == R_BADMEMSTATE);
    CHECK(g_phys_mapped == 0x1000);
    CHECK(mem_set_attr(a, 0x1000, 1, 0) == 0);
    /* Unlike a heap shrink, physical unmapping permits a normal page whose permissions changed. */
    CHECK(mem_set_state(a, 0x1000, MS_NORMAL, PERM_R) == 0);
    CHECK(svc_unmap_physical(a, 0x1000) == 0);
    CHECK(g_phys_mapped == 0);
}
static void test_locked_shrink(void) {
    base_process();
    CHECK(svc_set_heap_size(4 * MIB) == 0);
    CHECK(mem_set_attr(HEAP_BASE + 2 * MIB, 0x1000, 1, 1) == 0);
    CHECK(svc_set_heap_size(2 * MIB) == R_BADMEMSTATE);
    CHECK(g_heap_size == 4 * MIB && state(HEAP_BASE + 3 * MIB) == MS_NORMAL);
    CHECK(mem_set_attr(HEAP_BASE + 2 * MIB, 0x1000, 1, 0) == 0);
    CHECK(mem_set_state(HEAP_BASE + 2 * MIB, 0x1000, MS_NORMAL, PERM_R) == 0);
    CHECK(svc_set_heap_size(2 * MIB) == R_BADMEMSTATE);
    CHECK(mem_set_state(HEAP_BASE + 2 * MIB, 0x1000, MS_NORMAL, PERM_RW) == 0);
    CHECK(svc_set_heap_size(2 * MIB) == 0);
    CHECK(svc_set_heap_size(0) == 0);
}
static int ready, go;
typedef struct {
    int physical;
    u32 result;
} Request;
static void* race_request(void* arg) {
    Request* q = arg;
    __atomic_add_fetch(&ready, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&go, __ATOMIC_SEQ_CST)) sched_yield();
    q->result = q->physical ? svc_map_physical(ALIAS_BASE, 2 * MIB) : svc_set_heap_size(2 * MIB);
    return NULL;
}
static void test_shared_budget(void) {
    base_process();
    g_hle.kernel.sysres_size = 2 * MIB;
    g_code_memory = mem_total() - g_main_stack_size - g_hle.kernel.sysres_size - 2 * MIB;
    for (int i = 0; i < 50; i++) {
        ready = go = 0;
        Request q[2] = {{0, ~0u}, {1, ~0u}};
        pthread_t t[2];
        CHECK(pthread_create(&t[0], NULL, race_request, &q[0]) == 0);
        CHECK(pthread_create(&t[1], NULL, race_request, &q[1]) == 0);
        while (__atomic_load_n(&ready, __ATOMIC_SEQ_CST) != 2) sched_yield();
        __atomic_store_n(&go, 1, __ATOMIC_SEQ_CST);
        CHECK(pthread_join(t[0], NULL) == 0 && pthread_join(t[1], NULL) == 0);
        CHECK((q[0].result == 0 && q[1].result == R_LIMIT) || (q[0].result == R_LIMIT && q[1].result == 0));
        CHECK(used_memory() == mem_total());
        CHECK(svc_set_heap_size(0) == 0);
        CHECK(svc_unmap_physical(ALIAS_BASE, 2 * MIB) == 0);
        CHECK(g_heap_size == 0 && g_phys_mapped == 0);
    }
}
static void test_mapping_boundary(void) {
    base_process();
    g_gpu_mapping_begin = mapping_begin;
    g_gpu_mapping_end = mapping_end;
    CHECK(mem_map(ALIAS_BASE, 0x1000, MS_SHARED, PERM_RW) == 0);
    CHECK(mem_unmap(ALIAS_BASE, 0x1000) == 0);
    CHECK(mem_heap_set(0x2000, mem_total()) == 0);
    CHECK(mem_heap_set(0x2000, mem_total()) == 0);
    CHECK(mem_heap_set(0, mem_total()) == 0);
    CHECK(mapping_begins == 4 && mapping_ends == 4 && mapping_depth == 0);
    g_gpu_mapping_begin = NULL;
    g_gpu_mapping_end = NULL;
}
/* MapMemory's view is a copy of the locked source, copied back on unmap; QueryMemory reports it */
static void test_map_memory(void) {
    u64 src, dst = STACK_BASE + 0x10000, info = STACK_BASE;
    base_process();
    CHECK(svc_set_heap_size(2 * MIB) == 0);
    src = HEAP_BASE + 0x3000;
    CHECK(mem_map(info, 0x1000, MS_STACK, PERM_RW) == 0);
    gw32(src, 0xCAFE);
    CHECK(svc_map_memory(dst + 1, src, 0x1000) == R_BADADDR);
    CHECK(svc_map_memory(dst, src, 0) == R_BADSIZE);
    CHECK(svc_map_memory(HEAP_BASE, src, 0x1000) == R_BADREGION);
    CHECK(svc_map_memory(dst, src, 0x2000) == 0);
    CHECK(gr32(dst) == 0xCAFE && state(dst) == MS_STACK);
    CHECK(svc_query(info, src) == 0);
    CHECK(gr64(info) == src && gr64(info + 8) == 0x2000 && gr32(info + 16) == (MS_NORMAL & 0xFF));
    CHECK(gr32(info + 20) == 1 && gr32(info + 24) == PERM_NONE); /* locked, no permission */
    CHECK(svc_map_memory(dst + 0x4000, src, 0x1000) == R_BADMEMSTATE);
    CHECK(svc_set_heap_size(0) == R_BADMEMSTATE);
    gw32(dst + 0x1000, 0xBEEF);
    CHECK(svc_unmap_memory(dst, src, 0x2000) == 0);
    CHECK(gr32(src + 0x1000) == 0xBEEF && state(dst) == MS_FREE);
    /* the alias region takes the view too, as normal memory */
    CHECK(svc_map_memory(ALIAS_BASE, src, 0x1000) == 0 && state(ALIAS_BASE) == MS_NORMAL && gr32(ALIAS_BASE) == 0xCAFE);
    CHECK(svc_unmap_memory(dst, src, 0x1000) == R_BADMEMSTATE); /* not where it is mapped */
    CHECK(svc_unmap_memory(ALIAS_BASE, src, 0x1000) == 0 && state(ALIAS_BASE) == MS_FREE && state(src) == MS_NORMAL);
    CHECK(svc_query(info, AS_END + 5) == 0 && gr64(info) == AS_END && gr32(info + 16) == MS_INACCESSIBLE);
    CHECK(svc_query(0, HEAP_BASE) == 0xE601);
    CHECK(svc_set_mem_perm(src, 0x1000, PERM_RX) == 0xD801);
    CHECK(svc_set_mem_perm(src, 0x1000, PERM_R) == 0);
    CHECK(svc_set_mem_attr(src, 0x1000, 1, 1) == R_BADCOMBO);
    CHECK(svc_set_mem_attr(src, 0x1000, 8, 8) == 0);
    CHECK(svc_set_mem_attr(src, 0x1000, 8, 0) == 0);
    CHECK(svc_set_mem_perm(src, 0x1000, PERM_RW) == 0);
    CHECK(mem_unmap(info, 0x1000) == 0);
    CHECK(svc_set_heap_size(0) == 0);
}
/* Guest pages sharing a host page: a mapping commits the whole host page, an unmapping zeroes its own pages while a
   neighbour maps the rest, and decommits the host page with the last of them, so a page mapped again reads zero */
static void test_host_pages(void) {
    u64 a = ALIAS_BASE + 0x200000, b = a + 0x1000;
    int before;
    base_process();
    CHECK(mem_map(a, 0x1000, MS_SHARED, PERM_RW) == 0 && mem_map(b, 0x1000, MS_SHARED, PERM_RW) == 0);
    gw32(a, 0x11111111);
    gw32(b, 0x22222222);
    before = decommit_calls;
    CHECK(mem_unmap(a, 0x1000) == 0 && state(a) == MS_FREE);
    CHECK(gr32(b) == 0x22222222);
    if (host_page > 0x1000) {
        CHECK(decommit_calls == before); /* b still maps the host page: a's part is zeroed instead */
        CHECK(gr32(a) == 0);
    }
    CHECK(mem_map(a, 0x1000, MS_SHARED, PERM_RW) == 0 && gr32(a) == 0);
    CHECK(mem_unmap(a, 0x2000) == 0 && state(b) == MS_FREE);
    CHECK(decommit_calls > before);
    CHECK(mem_map(b, 0x1000, MS_SHARED, PERM_RW) == 0 && gr32(b) == 0);
    if (host_page > 0x1000) CHECK(gr32(a) == 0); /* free, but in b's host page: committed, and zero */
    CHECK(mem_unmap(b, 0x1000) == 0);
}
/* the runs a GPU read keeps to (mem_mapped_run): mapped pages up to the first free one across blocks of other states,
   none from a free one, none at the address space's end; a run the thread kept is dropped when a page of it goes */
static void test_mapped_runs(void) {
    u64 a = ALIAS_BASE + 0x400000;
    base_process();
    CHECK(mem_map(a, 0x2000, MS_SHARED, PERM_RW) == 0 && mem_map(a + 0x2000, 0x1000, MS_STACK, PERM_RW) == 0);
    CHECK(mem_mapped_run(a, 0x3000) == 0x3000 && mem_mapped_run(a + 0x10, 0x100) == 0x100);
    CHECK(mem_mapped_run(a + 0x2000, 0x2000) == 0x1000 && mem_mapped_run(a, 0) == 0);
    CHECK(mem_mapped_run(a, 0x3000) == 0x3000); /* the run this thread keeps, until a page of it goes */
    CHECK(mem_unmap(a + 0x1000, 0x1000) == 0);
    CHECK(mem_mapped_run(a + 0x1000, 0x10) == 0);
    CHECK(mem_mapped_run(a, 0x3000) == 0x1000 && mem_mapped_run(a + 0x1000, 0x2000) == 0);
    CHECK(mem_mapped_run(a + 0x2000, 0x1000) == 0x1000);
    CHECK(mem_mapped_run(AS_END, 0x1000) == 0 && mem_mapped_run(AS_END - 0x1000, ~0ull) <= 0x1000);
    CHECK(mem_unmap(a, 0x1000) == 0 && mem_unmap(a + 0x2000, 0x1000) == 0);
    CHECK(mem_mapped_run(a, 0x3000) == 0);
}
/* a service's shared memory: its host buffer until the game maps it, the guest's pages while mapped */
static void test_shared_memory(void) {
    KShmem* s = kshmem_new(0x2000, "test");
    u64 a = ALIAS_BASE - 0x100000;
    base_process();
    test_handle = &s->o;
    kshmem_ptr(s)[0] = 7;
    CHECK(svc_map_shm(0x1234, HEAP_BASE, 0x2000, PERM_R) == R_BADREGION);
    /* another size than the object's (InvalidSize on the console) maps the whole object, and unmaps all of it */
    CHECK(svc_map_shm(0x1234, a, 0x1000, PERM_R) == 0 && s->gsize == 0x2000 && state(a + 0x1000) == MS_SHARED);
    CHECK(svc_unmap_shm(0x1234, a, 0x1000) == 0 && state(a) == MS_FREE && state(a + 0x1000) == MS_FREE);
    CHECK(svc_map_shm(0x1234, a, 0x2000, PERM_RX) == 0xD801);
    CHECK(svc_map_shm(0x1235, a, 0x2000, PERM_R) == R_BADHANDLE);
    CHECK(svc_map_shm(0x1234, a, 0x2000, PERM_R) == 0);
    CHECK(GP(a)[0] == 7 && kshmem_ptr(s) == GP(a) && state(a) == MS_SHARED);
    CHECK(svc_map_shm(0x1234, a + 0x10000, 0x2000, PERM_R) == R_BADSTATE);
    kshmem_ptr(s)[1] = 9;
    CHECK(svc_unmap_shm(0x1234, a + 0x1000, 0x2000) == R_BADMEMSTATE);
    CHECK(svc_unmap_shm(0x1234, a, 0x2000) == 0);
    CHECK(kshmem_ptr(s) == s->host && s->host[1] == 9 && state(a) == MS_FREE);
    test_handle = NULL;
    kshmem_destroy(&s->o);
}

int main(void) {
    hle_init();
    /* this host's pages, then those of hosts with larger ones (16 KiB: Apple silicon, some Android kernels; 64 KiB:
       PowerPC64 and some AArch64 distributions), which mem.c commits in whole */
    static const u64 pages[] = {16 << 10, 64 << 10};
    unsigned i;
    host_page = (u64)sysconf(_SC_PAGESIZE);
    for (i = 0; i <= sizeof pages / sizeof *pages; i++) {
        if (i) {
            if (pages[i - 1] <= host_page) continue;
            host_page = pages[i - 1];
        }
        mapping_begins = mapping_ends = 0; /* test_mapping_boundary's, from the round before */
        mem_init();
        test_heap();
        test_system_resource();
        test_rollback();
        test_locked_shrink();
        test_shared_budget();
        test_map_memory();
        test_shared_memory();
        test_host_pages();
        test_mapped_runs();
        test_mapping_boundary(); /* last: every commit after it must be inside a mapping */
        CHECK(munmap(reservation, reservation_size) == 0);
        printf("memory accounting with %llu KiB host pages: %u checks passed\n",
               (unsigned long long)(host_page >> 10), checks);
    }
    return 0;
}
