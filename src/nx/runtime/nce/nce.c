/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NCE in the runtime (nce.h): the patches after the load, the outcalls through RtAPI, the hooks' entries, the kicks
   for CPU.poll and the faults of guest code */
#include "nx/runtime/nce/nce.h"
#include "nx/runtime/nce/nce_host.h"
#include "nx/runtime/nce/nce_patch.h"
#include "common/utilities/common.h"
#include "nx/hle/hle.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/memory/mem.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/guest_stats.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/tracy.h"
#include "snx_settings.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the functions whose entries branch to an outcall: a hook's, and NVN's, which take one function each */
#define NCE_DIVERTS 4096
/* how often a kick goes again to a thread whose CPU.poll is still set: one may land in the outcall's own code */
#define NCE_KICK_AGAIN_NS 2000000

static struct {
    int checked, file, wanted, on;
    char why[240]; /* why a game of an NCE recompile does not get it on this host (nce_host_check) */
    NceEnv env;
    PMutex lock; /* the pools and the diverts, which a guest thread changes (ldr:ro, NVN's resolver) */
    NcePool* pool;
    size_t pools;
    u64 trap; /* where a run that the runtime starts returns: nce_native */
    u64 entry[NCE_DIVERTS], original[NCE_DIVERTS];
    u8 family[NCE_DIVERTS]; /* each one's family, which the log counts its outcalls by (guest_stats_family) */
    u32 diverts;
    /* each thread's host id while it runs native code, and 1 while that code is the guest's, not the runtime's: a
       kick then stops it. By the thread's index (kt_ix) */
    int tid[KTHREAD_MAX + 1];
    u8 guest[KTHREAD_MAX + 1];
} g_nce = {.lock = PMUTEX_INIT};

int nce_on(void) { return g_nce.on; }

const SnxModule* nce_module(const SnxModule* m) {
    u64 by = mem_moved();
    size_t mods = m->nmods * sizeof *m->mods, entries = m->nentries * sizeof *m->entry_addr,
           fns = m->nfns * sizeof *m->fn_lo;
    u8* block;
    SnxModule* moved;
    SnxModInfo* mi;
    u64 *entry_addr, *fn_lo;
    if (!by) return m;
    /* one block: the descriptor, its modules, its entries' and its functions' addresses, each a multiple of 8 */
    block = (u8*)snx_alloc(sizeof *moved + mods + entries + fns);
    moved = (SnxModule*)(void*)block;
    mi = (SnxModInfo*)(void*)(block + sizeof *moved);
    entry_addr = (u64*)(void*)(block + sizeof *moved + mods);
    fn_lo = (u64*)(void*)(block + sizeof *moved + mods + entries);
    *moved = *m;
    for (u32 i = 0; i < m->nmods; i++) {
        mi[i] = m->mods[i];
        mi[i].base += by;
    }
    for (u32 i = 0; i < m->nentries; i++) entry_addr[i] = m->entry_addr[i] + by;
    for (u32 i = 0; i < m->nfns; i++) fn_lo[i] = m->fn_lo[i] + by;
    moved->mods = mi;
    moved->entry_addr = entry_addr;
    moved->fn_lo = fn_lo;
    if (moved->code_hi > moved->code_lo) {
        moved->code_lo += by;
        moved->code_hi += by;
    }
    LOG("nce: the game's modules moved %#llx up with the guest's addresses: the first at %#llx\n",
        (unsigned long long)by, (unsigned long long)(m->nmods ? mi[0].base : 0));
    return moved;
}

#if !SNX_NCE_HOST
int nce_wanted(void) { return 0; }
void nce_init(void) {}
void nce_thread_run(struct KThread* t, u64 pc) {
    (void)t;
    (void)pc;
    fatal("NCE runs only on an AArch64 Linux host");
}
void nce_kick(struct KThread* t) { (void)t; }
GuestFn nce_slot(GuestFn f) { return f; }
void nce_slot_taken(u64 addr, const char* family) {
    (void)addr;
    (void)family;
}
void nce_code_added(u64 base, u64 text_size) {
    (void)base;
    (void)text_size;
}
void nce_crash_context(const void* context) { (void)context; }
#else

static void file_path(char* out, size_t size) { snprintf(out, size, "%s%s.nce", g_exedir, g_exename); }

int nce_wanted(void) {
    if (!g_nce.checked) {
        char path[sizeof g_exedir + sizeof g_exename + 8];
        file_path(path, sizeof path);
        g_nce.file = snx_is_file(path);
        g_nce.wanted = g_nce.file && !nce_host_check(g_nce.why, sizeof g_nce.why);
        g_nce.checked = 1;
    }
    return g_nce.wanted;
}

static void set_guest(const CPU* c, u8 v) { __atomic_store_n(&g_nce.guest[kt_ix((KThread*)c->thread)], v, __ATOMIC_RELEASE); }

/* a word of the game's code, written where the host's CPU fetches it */
static void code_write(u64 a, u32 word) {
    __atomic_store_n((u32*)(void*)GP(a), snx_le32(word), __ATOMIC_RELEASE);
    __builtin___clear_cache((char*)GP(a), (char*)GP(a + 4));
}

/* ---- the runtime's side of the stubs ---- */

/* the native code that a slot means when it is empty, or holds this: the function from its own first word, which a
   divert moved to its stub. It returns to the trap, which ends the run */
static void nce_native(CPU* c, u64 entry) {
    u64 back = c->x[30], pc = entry;
    for (u32 i = 0, n = __atomic_load_n(&g_nce.diverts, __ATOMIC_ACQUIRE); i < n; i++)
        if (g_nce.entry[i] == entry) pc = g_nce.original[i];
    c->x[30] = g_nce.trap;
    c->pc = pc;
    guest_stats_add(kt_ix((KThread*)c->thread), GSTAT_NCE_RUN);
    set_guest(c, 1);
    nce_host_run(c);
    set_guest(c, 0);
    c->x[30] = back;
    c->pc = back; /* the GuestFn's return: where the caller goes on */
}

GuestFn nce_slot(GuestFn f) { return f || !g_nce.on ? f : nce_native; }

/* a hook's outcall: the slot's handler, or the function's own code. The trap ends the innermost run */
static void hook(CPU* c, u32 ix, unsigned index) {
    GuestFn f;
    u64 entry;
    if (index == NCE_TRAP) {
        guest_stats_add(ix, GSTAT_NCE_TRAP);
        nce_host_leave();
    }
    if (index >= __atomic_load_n(&g_nce.diverts, __ATOMIC_ACQUIRE))
        fatal("NCE: an outcall of hook %u, which no function has", index);
    guest_stats_hook(ix, g_nce.family[index]);
    entry = g_nce.entry[index];
    f = __atomic_load_n(&g_disp[(entry - g_code_lo) >> 2], __ATOMIC_ACQUIRE);
    if (!f || f == nce_native) {
        c->pc = g_nce.original[index];
        return;
    }
    c->pc = entry;
    f(c, entry); /* the adapter leaves the pc where the guest goes on: the function's return address */
}

static void dispatch(CPU* c, u64 code) {
    unsigned kind = (unsigned)(code >> 16), imm = (unsigned)(code & 0xFFFFu);
    u32 ix = kt_ix((KThread*)c->thread);
    set_guest(c, 0);
    switch (kind) {
    case NCE_SVC:
        guest_stats_add(ix, GSTAT_NCE_SVC);
        if (imm < SNX_SVC_COUNT && c->rt->svcs)
            c->rt->svcs[imm](c, imm);
        else
            c->rt->svc(c, imm);
        break;
    case NCE_TICKS:
        guest_stats_add(ix, GSTAT_NCE_TICKS);
        c->x[imm & 31u] = c->rt->ticks();
        break;
    case NCE_HOOK: hook(c, ix, imm); break;
    default: fatal("NCE: an outcall of kind %u, which no stub makes", kind);
    }
    if (__atomic_load_n(&c->poll, __ATOMIC_RELAXED)) c->rt->poll(c);
    set_guest(c, 1);
}

static void preempted(CPU* c) {
    guest_stats_add(kt_ix((KThread*)c->thread), GSTAT_NCE_KICK);
    set_guest(c, 0);
    c->rt->poll(c);
    set_guest(c, 1);
}

/* an instruction that the guest's code cannot run (UDF, or one the host refuses at EL0), or a BRK: RtAPI's undef and
   brk end the game with its state in the log */
static void trapped(CPU* c, int sig) {
    u32 w = gr32(c->pc);
    set_guest(c, 0);
    if (sig == SIGTRAP && (w & 0xFFE0001Fu) == 0xD4200000u)
        c->rt->brk(c, c->pc, (w >> 5) & 0xFFFFu);
    else
        c->rt->undef(c, c->pc, w);
    set_guest(c, 1);
}

/* ---- the kicks ---- */

static void kick_tid(u32 ix) {
    if (__atomic_load_n(&g_nce.guest[ix], __ATOMIC_ACQUIRE)) nce_host_kick(__atomic_load_n(&g_nce.tid[ix], __ATOMIC_ACQUIRE));
}

void nce_kick(struct KThread* t) {
    if (g_nce.on && t) kick_tid(kt_ix(t));
}

/* a kick that lands in the outcall's code, after its last look at CPU.poll and before the guest's code, stops nothing:
   this goes again to each thread that runs guest code with its poll still set */
static void kicker(void* arg) {
    (void)arg;
    SNX_THREAD_NAME("nce kicks");
    for (;;) {
        plat_sleep_ns(NCE_KICK_AGAIN_NS);
        for (u32 ix = 1; ix <= KTHREAD_MAX; ix++)
            if (__atomic_load_n(&g_nce.guest[ix], __ATOMIC_ACQUIRE) &&
                __atomic_load_n(&kt_at(ix)->cpu.poll, __ATOMIC_RELAXED))
                kick_tid(ix);
    }
}

void nce_thread_run(struct KThread* t, u64 pc) {
    CPU* c = &t->cpu;
    u32 ix = kt_ix(t);
    char why[240];
    __atomic_store_n(&g_nce.tid[ix], nce_host_tid(), __ATOMIC_RELEASE);
    if (nce_host_attach(c, why, sizeof why)) fatal("NCE: thread %u cannot run the game's code natively: %s", ix, why);
    if (!c->x[30]) c->x[30] = g_nce.trap; /* the entry function returns to 0: the end of the thread */
    c->pc = pc;
    set_guest(c, 1);
    nce_host_run(c);
    set_guest(c, 0);
    __atomic_store_n(&g_nce.tid[ix], 0, __ATOMIC_RELEASE);
}

void nce_crash_context(const void* context) {
    KThread* t = g_nce.on ? kthread_cur() : NULL;
    if (t) nce_host_context(context, &t->cpu);
}

/* ---- the patches ---- */

/* a pool in guest memory, which the guest's allocations then leave alone */
static int pool_add(u64 addr, u64 size) {
    NcePool* p;
    if (mem_map(addr, size, MS_CODE, PERM_RX)) return -1;
    g_nce.pool = (NcePool*)snx_realloc(g_nce.pool, (g_nce.pools + 1) * sizeof *g_nce.pool);
    p = &g_nce.pool[g_nce.pools++];
    p->buf = GP(addr);
    p->addr = addr;
    p->size = size;
    p->used = 0;
    return 0;
}

/* each site's stub and patch. A site whose word is not the one the file names (another module than the recompile's)
   or that no pool reaches would run as it is: an SVC would be a system call of the host. The game cannot run then */
static void patch_sites(const NceSites* s) {
    for (size_t i = 0; i < s->count; i++) {
        const NceSite* site = &s->site[i];
        u32 word;
        if (gr32(site->addr) != site->word)
            fatal("NCE: the game's code at %llx is %08x, where its recompile found %08x. Recompile the game",
                  (unsigned long long)site->addr, gr32(site->addr), site->word);
        if (nce_emit(g_nce.pool, g_nce.pools, &g_nce.env, site, &word))
            fatal("NCE: no pool of stubs is in the reach of the game's code at %llx", (unsigned long long)site->addr);
        code_write(site->addr, word);
    }
    for (size_t i = 0; i < g_nce.pools; i++)
        __builtin___clear_cache((char*)g_nce.pool[i].buf, (char*)g_nce.pool[i].buf + g_nce.pool[i].used);
}

void nce_init(void) {
    char path[sizeof g_exedir + sizeof g_exename + 8], why[160];
    NceHostConfig config = {dispatch, preempted, trapped, AS_BASE, AS_END};
    NceRange* ranges;
    NceSites sites;
    size_t count;
    if (g_arm_mod) return;
    if (!nce_wanted()) {
        if (g_nce.file) WARN("nce: %s, so the JIT compiles all of the game's code\n", g_nce.why);
        return;
    }
    if (!g_settings.nce) {
        WARN("nce: the game is of an NCE recompile and nce=0, so the JIT compiles all of its code: recompile it\n");
        return;
    }
    if (!mem_identity()) {
        WARN("nce: the guest's addresses are not the host's, so the JIT compiles all of the game's code\n");
        return;
    }
    file_path(path, sizeof path);
    if (!g_mod->nmods || nce_file_read(path, g_mod->mods[0].base, &ranges, &count, &sites))
        fatal("NCE: %s is of another version or damaged. Recompile the game", path);
    if (nce_host_init(&config, &g_nce.env.tls, &g_nce.env.indirect, &g_nce.env.outcall, why, sizeof why)) {
        WARN("nce: %s, so the JIT compiles all of the game's code\n", why);
        free(ranges);
        nce_sites_free(&sites);
        return;
    }
    for (size_t i = 0; i < count; i++)
        if (pool_add(ranges[i].addr, ranges[i].size))
            fatal("NCE: the pool of stubs at %llx (%llu KiB) cannot be mapped", (unsigned long long)ranges[i].addr,
                  (unsigned long long)(ranges[i].size >> 10));
    free(ranges);
    if (nce_trap(g_nce.pool, g_nce.pools, &g_nce.env, NCE_TRAP, &g_nce.trap)) fatal("NCE: no room for the trap");
    patch_sites(&sites);
    LOG("nce: the game's code runs natively: %zu words patched (%zu that the flow reaches), in %zu pools\n",
        sites.count, sites.proven, g_nce.pools);
    nce_sites_free(&sites);
    g_nce.on = 1;
    if (plat_thread(kicker, NULL, 64 << 10)) fatal("NCE: the thread of the kicks cannot start");
}

void nce_slot_taken(u64 addr, const char* family) {
    u64 original;
    u32 word, i;
    if (!g_nce.on) return;
    pm_lock(&g_nce.lock);
    for (i = 0; i < g_nce.diverts; i++)
        if (g_nce.entry[i] == addr) break;
    if (i < g_nce.diverts) {
        pm_unlock(&g_nce.lock);
        return;
    }
    if (i == NCE_DIVERTS) fatal("NCE: more than %d functions take handlers", NCE_DIVERTS);
    if (nce_divert(g_nce.pool, g_nce.pools, &g_nce.env, addr, gr32(addr), i, &word, &original)) {
        pm_unlock(&g_nce.lock);
        WARN("nce: the function at %llx keeps its own code: its first word cannot move, or no pool is in reach\n",
             (unsigned long long)addr);
        return;
    }
    g_nce.entry[i] = addr;
    g_nce.original[i] = original;
    g_nce.family[i] = (u8)guest_stats_family(family);
    for (size_t k = 0; k < g_nce.pools; k++)
        __builtin___clear_cache((char*)g_nce.pool[k].buf, (char*)g_nce.pool[k].buf + g_nce.pool[k].used);
    __atomic_store_n(&g_nce.diverts, i + 1, __ATOMIC_RELEASE); /* before the entry branches to it */
    code_write(addr, word);
    pm_unlock(&g_nce.lock);
}

void nce_code_added(u64 base, u64 text_size) {
    SnxCodeImage image = {0};
    SnxFunction whole = {0};
    SnxCodegen cg = {0};
    NceSites sites;
    u64 size, pool, reach = 120ull << 20;
    if (!g_nce.on) return;
    /* the analysis's view of it: one function over all of the text, from its first word, the NRO's branch to its
       start. Each word of the encodings that the flow does not prove to be data is patched */
    image.image = GP(base);
    image.image_size = (size_t)text_size;
    image.base = base;
    image.text_size = text_size;
    whole.start = base;
    whole.end = base + text_size;
    cg.modules = &image;
    cg.module_count = 1;
    cg.functions = &whole;
    cg.function_count = 1;
    nce_find_sites(&cg, &sites);
    size = nce_pool_size(&sites, base, base + text_size);
    pm_lock(&g_nce.lock);
    pool = mem_find_free(base + text_size, base + reach - size, size, 0x1000);
    if (!pool && base > reach) pool = mem_find_free(base + text_size - reach, base, size, 0x1000);
    if (!pool || pool_add(pool, size))
        fatal("NCE: no room for the stubs of the code that the game mapped at %llx", (unsigned long long)base);
    patch_sites(&sites);
    pm_unlock(&g_nce.lock);
    LOG("nce: the code at %llx: %zu words patched, its stubs at %llx\n", (unsigned long long)base, sites.count,
        (unsigned long long)pool);
    nce_sites_free(&sites);
}
#endif
