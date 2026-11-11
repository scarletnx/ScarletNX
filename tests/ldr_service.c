/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Real ldr:ro and memory block list; only platform and IPC transport are fakes.
   No game assets, RSA keys or precompiled NRO host libraries are required. */
#include "nx/memory/own.h"
#include "nx/hle/services/services_int.h"
#include "nx/gpu/gpu.h"
#include "nx/hle/hle.c"
#include "nx/runtime/nce/nce.h"
#include <errno.h>
#include <openssl/evp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "ldr_service:%d: %s\n", __LINE__, #x);                                                     \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)
#define RO(d) RC(22, d)
#define SRC   HEAP_BASE
#define BSS   (HEAP_BASE + 0x4000)
#define NRR   (HEAP_BASE + 0x5000)
#define SRC2  (HEAP_BASE + 0x8000)
static int fail_commit;
void (*g_gpu_mapping_begin)(u64, u64, int);
void (*g_gpu_mapping_end)(void);

void pm_init(PMutex* m) { CHECK(!pthread_mutex_init(&m->l, NULL)); }
void pm_lock(PMutex* m) { CHECK(!pthread_mutex_lock(&m->l)); }
void pm_unlock(PMutex* m) { CHECK(!pthread_mutex_unlock(&m->l)); }
void pm_lock_announce(PMutex* m, const char* name) { (void)m, (void)name; }
/* this host's pages, or the larger ones of --host_page=N (ldr_service_64k_pages), as plat_posix.c */
int g_ounce; /* an Ounce game's (main.c): none here */
static u64 g_host_page;
u64 plat_vm_page(void) {
    u64 page = (u64)sysconf(_SC_PAGESIZE);
    return g_host_page > page ? g_host_page : page;
}
/* aligned to plat_vm_page, a page more reserved for that */
u8* plat_vm_reserve(u64 size) {
    u64 page = plat_vm_page();
    void* p = mmap(NULL, size + page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : (u8*)(((uintptr_t)p + page - 1) & ~(uintptr_t)(page - 1));
}
void plat_vm_track(u8* p, u64 size) { (void)p, (void)size; } /* no tracking here */
int plat_ww_scan(u8* p, u64 size, void (*cb)(u8* p, u64 size)) {
    (void)p, (void)size, (void)cb;
    return -1;
}
/* the reservation without the write watch, which only Windows has: as plat_posix.c */
u8* plat_vm_reserve_unwatched(u64 size) {
    (void)size;
    errno = ENOSYS;
    return NULL;
}
/* mem.c's reservation at the guest's own addresses, which nce_wanted's 0 never takes: an AArch64 Linux host links it */
u8* plat_vm_reserve_at(u64 addr, u64 size) {
    (void)addr, (void)size;
    errno = ENOSYS;
    return NULL;
}
/* plat_posix.c's look at the maps, which mem_foreign asks only of NCE's reservation (platform_posix tests it) */
int plat_vm_foreign(u64 lo, u64 hi, int (*own)(u64 a, u64 b), char* out, size_t size) {
    (void)lo, (void)hi, (void)own;
    if (size) out[0] = 0;
    errno = ENOSYS;
    return -1;
}
/* nce.c, whose NCE no game here is of: mem.c's reservation asks it first, and ldr:ro gives it each NRO it maps */
int nce_wanted(void) { return 0; }
void nce_code_added(u64 base, u64 text_size) { (void)base, (void)text_size; }
int plat_vm_commit(u8* p, u64 size) {
    if (fail_commit) {
        fail_commit = 0;
        return -1;
    }
    return mprotect(p, size, PROT_READ | PROT_WRITE);
}
void plat_vm_decommit(u8* p, u64 size) {
    CHECK(!madvise(p, size, MADV_DONTNEED));
    CHECK(!mprotect(p, size, PROT_NONE));
}
/* runtime/guest_jit.c: what ldr:ro tells the JIT. -1: no JIT here */
static u64 jit_base, jit_text, jit_removed_base;
static int jit_added, jit_removed;
int jit_code_added(u64 base, u64 text_size, u64 size) {
    (void)size;
    jit_base = base;
    jit_text = text_size;
    ++jit_added;
    return -1;
}
void jit_code_removed(u64 base) {
    jit_removed_base = base;
    ++jit_removed;
}
/* main.c's table of the NROs the recompiler gave an address (nro_slots.h), and guest_jit.c's loading of an NRO's own
   AOT module: nro_answer is what it says of the next one (0 loaded, 1 none, -1 refused) */
const SnxNros* g_nros;
static u64 nro_base, nro_size, nro_removed_base;
static u8 nro_hash[32], nro_id[32];
static int nro_added, nro_removed, nro_answer;
int jit_nro_added(const u8* hash, u64 base, u64 size, const u8* id, u32* entries) {
    memcpy(nro_hash, hash, 32);
    memcpy(nro_id, id, 32);
    nro_base = base;
    nro_size = size;
    ++nro_added;
    *entries = nro_answer ? 0 : 7;
    return nro_answer;
}
void jit_nro_removed(u64 base) {
    nro_removed_base = base;
    ++nro_removed;
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
int plat_vm_share(u8* p, int fd, u64 size) {
    (void)p, (void)fd, (void)size;
    return -1;
}
int plat_vm_unshare(u8* p, u64 size) {
    (void)p, (void)size;
    return -1;
}
void plat_freeze(u8* p, u64 size) { (void)p, (void)size; }
void plat_thaw(void) {}
void gpu_mem_changed(u64 a, u64 size) {
    (void)a;
    (void)size;
}
/* the service's last two lines, for the checks of what it says of an NRO's mapping */
static char said[2][512];
void logf_(int level, const char* fmt, ...) {
    va_list ap;
    (void)level;
    memcpy(said[0], said[1], sizeof said[0]);
    va_start(ap, fmt);
    vsnprintf(said[1], sizeof said[1], fmt, ap);
    va_end(ap);
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    abort();
}
static KObj process = {.type = KO_PROCESS};
KObj* h_get_ref(u32 h, int type) { return h == 0xFFFF8001u && type == KO_PROCESS ? &process : NULL; }
void kobj_unref(KObj* p) { CHECK(p == &process); }
SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*)) {
    SvcObj* o = calloc(1, sizeof *o);
    CHECK(o != NULL);
    o->name = name;
    o->dispatch = dispatch;
    o->refs = 1;
    return o;
}
#include "svcobj_data.h"
void svcobj_unref(SvcObj* o) {
    if (--o->refs) return;
    if (o->destroy) o->destroy(o);
    test_svcobj_data_free(o);
    free(o);
}
void ipc_out_overflow(IpcCtx* c) {
    (void)c;
    CHECK(0);
}
/* sf's InvalidCmifRequest, for a request that doesn't send what its command declares */
int ipc_bad_request(SvcObj* o, IpcCtx* c) {
    (void)o;
    (void)c;
    return RC(11, 420);
}
/* sf's InvalidNumInObjects, for a domain request with another number of objects */
int ipc_bad_objects(SvcObj* o, IpcCtx* c) {
    (void)o;
    (void)c;
    return RC(10, 235);
}
int ipc_unknown(SvcObj* o, IpcCtx* c) {
    (void)o;
    (void)c;
    return RC(10, 221);
}
void svc_register(const char* name, SvcCtor ctor) {
    (void)name;
    (void)ctor;
}
static IpcCtx ctx;
static u64 raw[5];
static void request(u32 cmd, u64 a, u64 b, u64 c, u64 d) {
    memset(&ctx, 0, sizeof ctx);
    raw[0] = 0xdeadbeef; /* raw PID placeholder: not the trusted descriptor PID */
    raw[1] = a;
    raw[2] = b;
    raw[3] = c;
    raw[4] = d;
    snx_le64_array(raw, 5); /* as the guest sends them */
    ctx.cmd = cmd;
    ctx.raw = (const u8*)raw;
    ctx.raw_size = sizeof raw;
    ctx.has_pid = 1;
    ctx.pid = K_PROCESS_ID;
    ctx.ncopy = cmd == 4 || cmd == 10; /* the commands that take the process handle */
    ctx.copy_h[0] = 0xFFFF8001u;
}
static int call(SvcObj* o) { return o->dispatch(o, &ctx); }
static u64 output(void) {
    CHECK(ctx.out_size == 8);
    return snx_ld_le64(ctx.out);
}
static void block_is(u64 a, u32 state, u32 perm, u32 attr) {
    MemBlock b;
    mem_query(a, &b);
    CHECK(b.state == state && b.perm == perm && b.attr == attr);
}
static void fixture(void) {
    memset(GP(SRC), 0, 0x6000);
    memcpy(GP(SRC + 0x10), "NRO0", 4);
    gw32(SRC + 0x18, 0x3000);
    gw32(SRC + 0x24, 0x1000);
    gw32(SRC + 0x28, 0x1000);
    gw32(SRC + 0x2c, 0x1000);
    gw32(SRC + 0x30, 0x2000);
    gw32(SRC + 0x34, 0x1000);
    gw32(SRC + 0x38, 0x1000);
    memset(GP(SRC + 0x40), 0x42, 32);
    /* A function absent from any AOT dispatch: mov x0,#42; ret. */
    gw32(SRC + 0x80, 0xd2800540);
    gw32(SRC + 0x84, 0xd65f03c0);
    /* A relative relocation witness. The service must leave it for the guest linker, not apply it itself. */
    gw32(SRC + 4, 0x100);
    memcpy(GP(SRC + 0x100), "MOD0", 4);
    gw32(SRC + 0x104, 0x100);
    gw64(SRC + 0x200, 7);
    gw64(SRC + 0x208, 0x280);
    gw64(SRC + 0x210, 8);
    gw64(SRC + 0x218, 24);
    gw64(SRC + 0x220, 9);
    gw64(SRC + 0x228, 24);
    gw64(SRC + 0x280, 0x2000);
    gw64(SRC + 0x288, 1027);
    gw64(SRC + 0x290, 0x80);
    gw64(SRC + 0x2000, 0x80);
    memset(GP(BSS), 0xa5, 0x1000); /* map preserves caller's BSS backing, as Eden does */
    memcpy(GP(NRR), "NRR0", 4);
    gw64(NRR + 0x330, g_hle.kernel.title_id);
    gw32(NRR + 0x338, 0x1000);
    gw32(NRR + 0x340, 0x350);
    gw32(NRR + 0x344, 1);
    CHECK(EVP_Digest(GP(SRC), 0x3000, GP(NRR + 0x350), NULL, EVP_sha256(), NULL));
    memcpy(GP(SRC2), GP(SRC), 0x3000);
}
static void initialize(SvcObj* o) {
    request(4, 0, 0, 0, 0);
    CHECK(!call(o));
}
static u64 load(SvcObj* o) {
    request(0, SRC, 0x3000, BSS, 0x1000);
    CHECK(!call(o));
    return output();
}
/* the two loads and the main thread start together, at a gate the three wait at once: a barrier, which Darwin's
   pthreads don't have */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t open;
    int waiting;
} Gate;
static void gate_wait(Gate* g) {
    pthread_mutex_lock(&g->lock);
    if (++g->waiting == 3)
        pthread_cond_broadcast(&g->open);
    else
        while (g->waiting < 3) pthread_cond_wait(&g->open, &g->lock);
    pthread_mutex_unlock(&g->lock);
}
typedef struct {
    SvcObj* service;
    Gate* gate;
    u64 source, bss, base;
    int result;
} LoadRace;
static void* race_load(void* arg) {
    LoadRace* r = arg;
    u64 args[] = {0, r->source, 0x3000, r->bss, 0x1000};
    IpcCtx c = {.cmd = 0, .raw = (const u8*)args, .raw_size = sizeof args, .has_pid = 1, .pid = K_PROCESS_ID};
    snx_le64_array(args, 5);
    gate_wait(r->gate);
    r->result = r->service->dispatch(r->service, &c);
    if (!r->result) r->base = snx_ld_le64(c.out);
    return NULL;
}
static void concurrency(void) {
    fixture();
    SvcObj* o = new_ldr();
    initialize(o);
    request(2, NRR, 0x1000, 0, 0);
    CHECK(!call(o));
    for (int repeat = 0; repeat < 8; repeat++) {
        Gate gate = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0};
        LoadRace a = {o, &gate, SRC, BSS, 0, 0}, b = {o, &gate, SRC2, SRC2 + 0x3000, 0, 0};
        pthread_t first, second;
        CHECK(!pthread_create(&first, NULL, race_load, &a));
        CHECK(!pthread_create(&second, NULL, race_load, &b));
        gate_wait(&gate);
        CHECK(!pthread_join(first, NULL));
        CHECK(!pthread_join(second, NULL));
        CHECK(!pthread_cond_destroy(&gate.open) && !pthread_mutex_destroy(&gate.lock));
        CHECK((!a.result && b.result == RO(3)) || (!b.result && a.result == RO(3)));
        request(1, a.base ? a.base : b.base, 0, 0, 0);
        CHECK(!call(o));
        block_is(SRC, MS_NORMAL, PERM_RW, 0);
        block_is(SRC2, MS_NORMAL, PERM_RW, 0);
    }
    svcobj_unref(o);
}
/* a request shorter than its command's arguments (0x28 bytes for MapManualLoadModuleMemory: the pid, then four u64s)
   is refused by sf before ldr sees it: nothing is mapped */
static void short_request(void) {
    fixture();
    SvcObj* o = new_ldr();
    initialize(o);
    request(0, SRC, 0x3000, BSS, 0x1000);
    ctx.raw_size = 0x20;
    CHECK(call(o) == RC(11, 420) && !ctx.out_size);
    block_is(SRC, MS_NORMAL, PERM_RW, 0);
    request(0, SRC, 0x3000, BSS, 0x1000);
    ctx.raw_size = 8;
    CHECK(call(o) == RC(11, 420));
    block_is(SRC, MS_NORMAL, PERM_RW, 0);
    svcobj_unref(o);
}
static void limits(void) {
    CHECK(!mem_heap_set(0x200000, 0x200000));
    fixture();
    SvcObj* o = new_ldr();
    initialize(o);
    for (unsigned i = 0; i < 64; i++) {
        u64 a = HEAP_BASE + 0x10000 + i * 0x1000;
        memcpy(GP(a), GP(NRR), 0x1000);
        request(2, a, 0x1000, 0, 0);
        CHECK(!call(o));
    }
    request(2, NRR, 0x1000, 0, 0);
    CHECK(call(o) == RO(8));
    svcobj_unref(o);
    o = new_ldr();
    initialize(o);
    /* 64 independently authorized NROs; a full table must not lock a 65th source. */
    gw32(NRR + 0x344, 64);
    for (unsigned i = 0; i < 64; i++) {
        u64 a = HEAP_BASE + 0x100000 + i * 0x4000;
        memcpy(GP(a), GP(SRC), 0x3000);
        GP(a + 0x40)[0] = (u8)i;
        CHECK(EVP_Digest(GP(a), 0x3000, GP(NRR + 0x350 + i * 32), NULL, EVP_sha256(), NULL));
    }
    request(2, NRR, 0x1000, 0, 0);
    CHECK(!call(o));
    for (unsigned i = 0; i < 64; i++) {
        u64 a = HEAP_BASE + 0x100000 + i * 0x4000;
        request(0, a, 0x3000, a + 0x3000, 0x1000);
        CHECK(!call(o));
    }
    request(0, SRC, 0x3000, BSS, 0x1000);
    CHECK(call(o) == RO(7));
    block_is(SRC, MS_NORMAL, PERM_RW, 0);
    svcobj_unref(o);
    CHECK(!mem_heap_set(0x10000, 0x200000));
}
/* NROs the recompiler gave an address (g_nros, nro_slots.h), as main.c hands them over: the fixture's at SLOT, in a
   range kept from AS_BASE on, the lowest addresses, where an NRO without a place would otherwise go */
#define SLOTS_END (AS_BASE + 0x100000)
#define SLOT      (AS_BASE + 0x6000)
static void aot_slots(void) {
    static SnxNroSlot slots[2];
    static SnxNros table = {SNX_NROS_VERSION, 2, 0, 0, slots};
    char hex[65], at[64], other[96];
    u8 second[32];
    u64 base, elsewhere;
    fixture();
    /* a second NRO, authorized but with no place in the table: the first with another module ID */
    GP(SRC2 + 0x40)[0] = 0x43;
    gw32(NRR + 0x344, 2);
    CHECK(EVP_Digest(GP(SRC2), 0x3000, GP(NRR + 0x370), NULL, EVP_sha256(), NULL));
    memcpy(second, GP(NRR + 0x370), 32);
    /* the table, in the order of the hashes: the fixture's, and one no NRO here has */
    memset(slots, 0, sizeof slots);
    memcpy(slots[1].hash, GP(NRR + 0x350), 32);
    slots[1].base = SLOT, slots[1].size = 0x4000;
    slots[0].base = AS_BASE + 0x1000, slots[0].size = 0x4000;
    CHECK(memcmp(slots[0].hash, slots[1].hash, 32) < 0);
    table.lo = AS_BASE;
    table.hi = SLOTS_END;
    g_nros = &table;
    mem_code_slots(table.lo, table.hi);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", slots[1].hash[i]);
    SvcObj* o = new_ldr();
    initialize(o);
    request(2, NRR, 0x1000, 0, 0);
    CHECK(!call(o));
    /* by its hash, at its address, its own module asked for with what is mapped, and the JIT told for the rest */
    nro_added = nro_removed = jit_added = jit_removed = nro_answer = 0;
    base = load(o);
    CHECK(base == SLOT && nro_added == 1 && nro_base == SLOT && nro_size == 0x4000 && jit_added == 1 &&
          jit_base == SLOT);
    CHECK(!memcmp(nro_hash, slots[1].hash, 32) && nro_id[0] == 0x42 && nro_id[31] == 0x42);
    snprintf(at, sizeof at, "mapped NRO at %llx ", (unsigned long long)SLOT);
    CHECK(strstr(said[1], at) && strstr(said[1], "its AOT module, 7 entries"));
    block_is(SLOT, MS_ALIASCODE, PERM_RX, 0);
    block_is(SLOT - 0x1000, MS_FREE, PERM_NONE, 0);
    CHECK(gr32(SLOT + 0x80) == 0xd2800540);
    /* unmapped, its module's entries go first; mapped again, it is at the same address */
    request(1, base, 0, 0, 0);
    CHECK(!call(o) && nro_removed == 1 && nro_removed_base == SLOT && jit_removed == 1);
    block_is(SLOT, MS_FREE, PERM_NONE, 0);
    CHECK(load(o) == SLOT && nro_added == 2);
    /* an NRO with no place goes outside the kept addresses, free as they are, and has no module */
    request(0, SRC2, 0x3000, SRC2 + 0x3000, 0x1000);
    CHECK(!call(o));
    elsewhere = output();
    CHECK(elsewhere >= SLOTS_END && nro_added == 2 && jit_added == 3 && strstr(said[1], "no AOT variants"));
    request(1, elsewhere, 0, 0, 0);
    CHECK(!call(o) && nro_removed == 2 && nro_removed_base == elsewhere);
    request(1, SLOT, 0, 0, 0);
    CHECK(!call(o));
    /* its address in use (other memory the game put there): mapped where there is room, for the JIT alone, and said
       with the hash and both addresses. Once the address is free it goes there again */
    CHECK(!mem_map(SLOT + 0x1000, 0x1000, MS_NORMAL, PERM_RW));
    nro_added = 0;
    base = load(o);
    CHECK(base >= SLOTS_END && !nro_added);
    snprintf(at, sizeof at, "mapped at %llx ", (unsigned long long)base);
    snprintf(other, sizeof other, " %llx, the address its AOT module is made for, is in use", (unsigned long long)SLOT);
    CHECK(strstr(said[0], hex) && strstr(said[0], at) && strstr(said[0], other) && strstr(said[0], "the JIT runs it"));
    CHECK(strstr(said[1], "no AOT variants"));
    request(1, base, 0, 0, 0);
    CHECK(!call(o));
    CHECK(!mem_unmap(SLOT + 0x1000, 0x1000));
    CHECK(load(o) == SLOT && nro_added == 1);
    request(1, SLOT, 0, 0, 0);
    CHECK(!call(o));
    /* a guard page short of free is in use too */
    CHECK(!mem_map(SLOT + 0x4000, 0x1000, MS_NORMAL, PERM_RW));
    CHECK(load(o) >= SLOTS_END);
    request(1, output(), 0, 0, 0);
    CHECK(!call(o));
    CHECK(!mem_unmap(SLOT + 0x4000, 0x1000));
    /* no module file, or one that is refused: at its address all the same, where the JIT compiles into the game's
       own dispatch table */
    for (nro_answer = 1; nro_answer >= -1; nro_answer -= 2) {
        nro_added = 0;
        CHECK(load(o) == SLOT && nro_added == 1 && strstr(said[1], "no AOT variants"));
        request(1, SLOT, 0, 0, 0);
        CHECK(!call(o));
    }
    nro_answer = 0;
    /* a place made for another size (the table of another version of the NRO's BSS): not this mapping's */
    slots[1].size = 0x5000;
    nro_added = 0;
    CHECK(load(o) >= SLOTS_END && !nro_added);
    request(1, output(), 0, 0, 0);
    CHECK(!call(o));
    slots[1].size = 0x4000;
    /* a place the address space has no code room at (in the heap region, a smaller layout's): as one in use */
    slots[1].base = HEAP_BASE + 0x100000;
    nro_added = 0;
    base = load(o);
    CHECK(base >= SLOTS_END && (base < HEAP_BASE || base >= HEAP_BASE + HEAP_SIZE) && !nro_added);
    CHECK(strstr(said[0], hex) && strstr(said[0], "is in use"));
    slots[1].base = SLOT;
    svcobj_unref(o);
    block_is(base, MS_FREE, PERM_NONE, 0);
    block_is(SRC, MS_NORMAL, PERM_RW, 0);
    g_nros = NULL;
    mem_code_slots(0, 0);
}
/* fn is g_ldr_state_fns' entry of that name: a save state names an ldr:ro object's dispatcher and destructor by it,
   and can't save a function no table names */
static int state_named(StateFnPtr fn, const char* name) {
    for (u32 i = 0; i < g_ldr_state_fns.n; i++)
        if (!strcmp(g_ldr_state_fns.fn[i].name, name)) return g_ldr_state_fns.fn[i].fn == fn;
    return 0;
}
int main(int argc, char** argv) {
    if (argc > 1) {
        CHECK(argc == 2 && !strncmp(argv[1], "--host_page=", 12));
        g_host_page = strtoull(argv[1] + 12, NULL, 0);
        CHECK(g_host_page > (u64)sysconf(_SC_PAGESIZE) && !(g_host_page & (g_host_page - 1)));
    }
    hle_init();
    mem_init();
    CHECK(!mem_heap_set(0x10000, 0x10000));
    fixture();
    SvcObj* o = new_ldr();
    request(0, SRC, 0x3000, BSS, 0x1000);
    CHECK(call(o) == RO(1031));
    request(4, 0, 0, 0, 0); /* no process id: sf refuses it before ldr sees it (InvalidCmifRequest) */
    ctx.has_pid = 0;
    CHECK(call(o) == RC(11, 420));
    request(4, 0, 0, 0, 0);
    ctx.copy_h[0] = 123;
    CHECK(call(o) == RO(1031));
    request(4, 0, 0, 0, 0); /* no process handle at all, or two: refused by sf as well */
    ctx.ncopy = 0;
    CHECK(call(o) == RC(11, 420));
    request(4, 0, 0, 0, 0);
    ctx.ncopy = 2;
    CHECK(call(o) == RC(11, 420));
    request(2, NRR, 0x1000, 0, 0); /* RegisterModuleInfo before RegisterProcessHandle */
    CHECK(call(o) == RO(1031));
    initialize(o);
    request(4, 0, 0, 0, 0);
    CHECK(call(o) == RO(1030));
    SvcObj* other = new_ro1();
    CHECK(state_named((StateFnPtr)o->dispatch, "ldr.ro_dispatch"));
    CHECK(state_named((StateFnPtr)other->dispatch, "ldr.ro_dispatch"));
    CHECK(state_named((StateFnPtr)o->destroy, "ldr.ro_destroy"));
    request(4, 0, 0, 0, 0);
    CHECK(call(other) == RO(1030));
    svcobj_unref(other);
    request(0, SRC, 0x3000, BSS, 0x1000);
    ctx.pid = 1;
    CHECK(call(o) == RO(1031));
    request(0, SRC + 1, 0x3000, BSS, 0x1000);
    CHECK(call(o) == RO(1025));
    request(0, SRC, 0, BSS, 0x1000);
    CHECK(call(o) == RO(1026));
    request(0, ~0xfffull, 0x3000, BSS, 0x1000);
    CHECK(call(o) == RO(1026));
    request(0, 0, 0x3000, 0, 0);
    CHECK(call(o) == RO(1027));
    request(0, AS_SIZE, 0x3000, 0, 0);
    CHECK(call(o) == RO(1027));
    request(0, SRC, 0x3000, SRC, 0x1000);
    CHECK(call(o) == RO(1027));
    request(0, HEAP_BASE + 0xe000, 0x3000, 0, 0);
    CHECK(call(o) == RO(1027)); /* source crosses into a hole */
    request(0, SRC, 0x3000, HEAP_BASE + 0x10000, 0x1000);
    CHECK(call(o) == RO(1027)); /* invalid BSS must not leave the image locked */
    block_is(SRC, MS_NORMAL, PERM_RW, 0);
    request(0, SRC, 0x3000, BSS, 0x1000);
    CHECK(call(o) == RO(9) && !ctx.out_size);
    block_is(SRC, MS_NORMAL, PERM_RW, 0);
    request(2, NRR, 0x1000, 0, 0);
    gw32(NRR + 0x344, ~0u);
    CHECK(call(o) == RO(6));
    gw32(NRR + 0x344, 1);
    gw32(NRR + 0x340, ~0u);
    CHECK(call(o) == RO(6));
    gw32(NRR + 0x340, 0x350);
    gw64(NRR + 0x330, 1);
    CHECK(call(o) == RO(6));
    gw64(NRR + 0x330, g_hle.kernel.title_id);
    CHECK(!call(o)); /* RegisterModuleInfo has no process handle: the session's RegisterProcessHandle gave it */
    /* Hash list is a snapshot. */
    memset(GP(NRR + 0x350), 0, 32);
    request(0, SRC, 0x3000, BSS, 0x1000);
    fail_commit = 1;
    CHECK(call(o) == RO(1023));
    block_is(SRC, MS_NORMAL, PERM_RW, 0);
    gw32(SRC + 0x24, 0xfff);
    request(0, SRC, 0x3000, BSS, 0x1000);
    CHECK(call(o) == RO(4));
    gw32(SRC + 0x24, 0x1000);
    gw32(SRC + 0x1c, 2);
    CHECK(call(o) == RO(4));
    gw32(SRC + 0x1c, 0);
    gw32(SRC + 0x38, 0);
    CHECK(call(o) == RO(4));
    gw32(SRC + 0x38, 0x1000);
    GP(SRC + 0x180)[0] = 1;
    CHECK(call(o) == RO(9));
    GP(SRC + 0x180)[0] = 0;
    u64 base = load(o);
    CHECK(base != SRC && !memcmp(GP(base), GP(SRC), 0x3000));
    CHECK(jit_added == 1 && jit_base == base && jit_text == 0x1000 && !jit_removed); /* its text joins the JIT */
    CHECK(gr64(base + 0x2000) == 0x80 && GP(base + 0x3000)[0] == 0xa5);
    block_is(base, MS_ALIASCODE, PERM_RX, 0);
    block_is(base + 0x1000, MS_ALIASCODE, PERM_R, 0);
    block_is(base + 0x2000, MS_ALIASCODEDATA, PERM_RW, 0);
    block_is(BSS, MS_NORMAL, PERM_NONE, 1);
    block_is(SRC, MS_NORMAL, PERM_NONE, 1);
    CHECK(g_code_memory == 0 && g_heap_size == 0x10000);
    CHECK(mem_heap_set(0, 0x10000) == MEM_BADSTATE);
    request(0, SRC2, 0x3000, SRC2 + 0x3000, 0x1000);
    CHECK(call(o) == RO(3));
    block_is(SRC2, MS_NORMAL, PERM_RW, 0);
    request(1, SRC, 0, 0, 0);
    CHECK(call(o) == RO(1028)); /* unload takes returned base, NOT source */
    request(1, base + 1, 0, 0, 0);
    CHECK(call(o) == RO(1025));
    gw64(base + 0x2008, 0xabcdef);
    gw64(base + 0x3000, 0x9876);
    request(3, NRR, 0, 0, 0);
    CHECK(!call(o)); /* unloading NRR must not unload NRO */
    request(1, base, 0, 0, 0);
    CHECK(!call(o));
    CHECK(jit_removed == 1 && jit_removed_base == base); /* before the unmap */
    CHECK(call(o) == RO(1028));
    block_is(base, MS_FREE, PERM_NONE, 0);
    block_is(SRC, MS_NORMAL, PERM_RW, 0);
    CHECK(gr64(SRC + 0x2008) == 0xabcdef && gr64(BSS) == 0x9876);
    request(999, 0, 0, 0, 0);
    CHECK(call(o) == RC(10, 221)); /* sf's UnknownCommandId */
    fixture();
    request(2, NRR, 0x1000, 0, 0);
    CHECK(!call(o));
    base = load(o);
    svcobj_unref(o);
    CHECK(jit_removed == 2 && jit_removed_base == base); /* the session's end unmaps it, and tells the JIT first */
    block_is(base, MS_FREE, PERM_NONE, 0);
    block_is(BSS, MS_NORMAL, PERM_RW, 0);
    o = new_ro1();
    initialize(o);
    fixture();
    request(10, NRR, 0x1000, 0, 0); /* RegisterProcessModuleInfo checks its process handle (sf, that there is one) */
    ctx.ncopy = 0;
    CHECK(call(o) == RC(11, 420));
    ctx.ncopy = 1;
    ctx.copy_h[0] = 123;
    CHECK(call(o) == RO(1031));
    request(10, NRR, 0x1000, 0, 0);
    CHECK(call(o) == RO(10));
    GP(NRR + 0x33c)[0] = 1;
    CHECK(!call(o));
    base = load(o);
    svcobj_unref(o);
    block_is(base, MS_FREE, PERM_NONE, 0);
    fixture();
    gw32(SRC + 0x38, 0);
    CHECK(EVP_Digest(GP(SRC), 0x3000, GP(NRR + 0x350), NULL, EVP_sha256(), NULL));
    o = new_ldr();
    initialize(o);
    request(2, NRR, 0x1000, 0, 0);
    CHECK(!call(o));
    request(0, SRC, 0x3000, 0, 0);
    CHECK(!call(o));
    base = output();
    block_is(BSS, MS_NORMAL, PERM_RW, 0); /* zero-sized BSS needs no backing */
    svcobj_unref(o);
    block_is(base, MS_FREE, PERM_NONE, 0);
    short_request();
    concurrency();
    aot_slots();
    limits();
    CHECK(!mem_heap_set(0, 0x10000));
    CHECK(!munmap(g_mem, AS_SIZE));
    puts("ldr:ro: IPC, authorization, mapping, rollback, unload, concurrency and capacity checks passed");
    return 0;
}
