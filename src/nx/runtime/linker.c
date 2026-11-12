/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/linker.h"
#include "common/utilities/common.h"
#include "nx/hle/hook/hook.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/loader/relocate.h"
#include "nx/memory/mem.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/nce/nce.h"
#include "nx/runtime/platform/log.h"
#include <stdlib.h>
#include <string.h>

/* nn::init::Start as the SDKs export it, with three callbacks of the linker's and with two: what a game's linker
   calls last. The route list names them (hle/hook/hooks.hook), so the module calls them through their slots */
static const char* const linker_start[] = {"_ZN2nn4init5StartEmmPFvvES2_S2_", "_ZN2nn4init5StartEmmPFvvES2_"};
/* a function of nn::ro the module must route for the runtime to answer the library (nn/ro.hook) */
#define LINKER_RO_ROUTED "_ZN2nn2ro12LookupSymbolEPmPKc"

typedef struct {
    SnxRelocModule* modules; /* one block: the modules, then, while the game's linker is watched, each one's copy */
    u32 count;
    SnxRelocCounts counts;
    const char* start_name;
    u64 start;        /* nn::init::Start, 0 when the game has none the runtime knows */
    int links;        /* the runtime linked the modules: the game's linker does not run */
    GuestFn original; /* what nn::init::Start's dispatch slot held, while the game's linker is watched */
    int done;
} Linker;
static Linker g_linker;

/* the game's modules for the linker: relocated in place, or (copy) into a copy of each one's data */
static void modules_make(int copy) {
    Linker* l = &g_linker;
    unsigned char* at;
    size_t bytes = 0;
    u32 i;
    for (i = 0; copy && i < g_mod->nmods; i++) bytes += g_mod->mods[i].data_size;
    l->count = g_mod->nmods;
    l->modules = (SnxRelocModule*)snx_alloc(l->count * sizeof(*l->modules) + bytes);
    at = (unsigned char*)(l->modules + l->count);
    for (i = 0; i < l->count; i++) {
        const SnxModInfo* mi = &g_mod->mods[i];
        SnxRelocModule* m = &l->modules[i];
        memset(m, 0, sizeof(*m));
        m->name = mi->name;
        m->image = GP(mi->base);
        m->base = mi->base;
        m->size = mi->size;
        m->data_off = mi->data_off;
        m->data_size = mi->data_size;
        m->data = copy ? at : GP(mi->base + mi->data_off);
        if (copy) {
            memcpy(at, GP(mi->base + mi->data_off), mi->data_size);
            at += mi->data_size;
        }
    }
}
static void modules_free(void) {
    free(g_linker.modules);
    g_linker.modules = NULL;
    g_linker.count = 0;
}
/* what nothing defines, said when there is any */
static void undefined_say(void) {
    const SnxRelocCounts* n = &g_linker.counts;
    if (n->absent || n->unresolved)
        LOG("linker: nothing defines the symbol of %llu words that are weak (0) and of %llu that are not (as "
            "loaded)%s%s\n",
            (unsigned long long)n->absent, (unsigned long long)n->unresolved, n->first_unresolved ? ", the first " : "",
            n->first_unresolved ? n->first_unresolved : "");
}

/* why the game's own linker links it, or NULL when the runtime can */
static const char* own_linker(void) {
    u32 i;
    if (nce_on()) return "the game runs its own code (NCE)";
    for (i = 0; i < sizeof linker_start / sizeof linker_start[0] && !g_linker.start; i++) {
        g_linker.start_name = linker_start[i];
        g_linker.start = kernel_find_symbol_exact(linker_start[i]);
    }
    if (!g_linker.start || g_linker.start < g_code_lo || g_linker.start >= g_code_hi || (g_linker.start & 3) ||
        !g_disp[(g_linker.start - g_code_lo) / 4]) {
        g_linker.start = 0;
        return "the game has no recompiled nn::init::Start";
    }
    if (g_mod->nmods < 2 || strcmp(g_mod->mods[0].name, "rtld") || g_mod->mods[0].text_size < 12)
        return "the game's first module is no rtld";
    if (hook_family_off(LINKER_FAMILY)) return "the " LINKER_FAMILY " hooks are off (hooks, hooks_off)";
    if (!hook_routed(LINKER_RO_ROUTED)) return "the module does not route nn::ro (recompile the game for it)";
    if (g_nros) return "the game maps NROs through nn::ro, which the runtime's linker does not load yet";
    for (i = 0; i < g_mod->nmods; i++)
        if ((u64)g_mod->mods[i].data_off + g_mod->mods[i].data_size > g_mod->mods[i].size)
            return "a module's data lies outside it";
    return NULL;
}

void linker_decide(void) {
    Linker* l = &g_linker;
    const char* why;
    char error[512];
    if (!g_mod || g_arm_mod) return; /* a 32-bit game's modules are ELF32: its own linker's */
    why = own_linker();
    if (!why) {
        modules_make(0);
        if (snx_relocate(l->modules, l->count, &l->counts, error, sizeof error)) { /* before any word is written */
            modules_free();
            LOG("linker: the game's own linker (rtld) links it: %s\n", error);
            return;
        }
        l->links = 1;
        LOG("linker: the runtime linked the game's %u modules itself, and the game's own linker (rtld) does not run: "
            "%llu relative relocations, %llu with a symbol, %llu PLT slots, each bound\n",
            (unsigned)l->count, (unsigned long long)l->counts.relative, (unsigned long long)l->counts.symbolic,
            (unsigned long long)l->counts.slots);
        undefined_say();
        return;
    }
    LOG("linker: the game's own linker (rtld) links it: %s\n", why);
}
int linker_links(void) { return g_linker.links; }

/* ---- the game's own linker, watched ---- */

/* the game's memory against the copy the runtime linked, said once */
static void compare(void) {
    Linker* l = &g_linker;
    SnxRelocDiff diff;
    char error[512];
    if (snx_relocate_compare(l->modules, l->count, &diff, error, sizeof error))
        WARN("linker: the game's own linker left a module the runtime's cannot read: %s\n", error);
    else if (diff.differ)
        WARN("linker: %llu of %llu relocated words differ from the runtime's linker's: the first in %s at %llx, where "
             "the game's own linker left %llx and the runtime's %llx\n",
             (unsigned long long)diff.differ, (unsigned long long)diff.words, l->modules[diff.module].name,
             (unsigned long long)(l->modules[diff.module].base + diff.at), (unsigned long long)diff.has,
             (unsigned long long)diff.wants);
    else
        LOG("linker: the game's own linker and the runtime's agree on %llu relocated words of %u modules (%llu "
            "relative, %llu with a symbol, %llu PLT slots, %llu of them not bound yet)\n",
            (unsigned long long)diff.words, (unsigned)l->count, (unsigned long long)l->counts.relative,
            (unsigned long long)l->counts.symbolic, (unsigned long long)l->counts.slots, (unsigned long long)diff.lazy);
    undefined_say();
    if (!diff.differ && (l->counts.outside || l->counts.other))
        LOG("linker: %llu relocations of a type the runtime's linker has no case for and %llu outside their module's "
            "data were not compared\n",
            (unsigned long long)l->counts.other, (unsigned long long)l->counts.outside);
    modules_free();
}

/* nn::init::Start's slot until its first call: the game's linker has relocated every module by then, and nothing
   else of the game has run */
static void start(CPU* c, u64 entry) {
    GuestFn original = g_linker.original;
    if (!__atomic_exchange_n(&g_linker.done, 1, __ATOMIC_ACQ_REL)) {
        __atomic_store_n(&g_disp[(g_linker.start - g_code_lo) / 4], original, __ATOMIC_RELEASE);
        compare();
    }
    original(c, entry);
}

/* ---- the runtime's linker in the game's linker's place ---- */

/* What a game's linker hands nn::init::Start: three functions the SDK calls, by their addresses. The runtime's are
   the first three instructions of the rtld's text, which stays mapped and never runs: their dispatch slots hold these.
   A guest function they call returns to the handler's own caller, an address nothing else returns to, as a callback
   of NVN's does (nvn_cmd.c's guest_call) */
static u64 callback(unsigned which) { return g_mod->mods[0].base + g_mod->mods[0].text_off + 4 * which; }
static void run(CPU* c, u64 fn, u64 lr) {
    u64 pc = fn;
    if (!fn) return;
    c->x[30] = lr;
    do snx_lookup(c, pc)(c, pc);
    while ((pc = c->pc) != lr);
}
/* the SDK's exception handler is ready: nothing of the runtime's waits for it */
static void ready(CPU* c, u64 pc) {
    (void)pc;
    c->pc = c->x[30];
}
/* each module's constructors (its DT_INIT), the last module first, as a game's linker runs them; not the rtld's */
static void init(CPU* c, u64 pc) {
    u64 lr = c->x[30];
    u32 i;
    (void)pc;
    for (i = g_linker.count; i-- > 1;) run(c, g_linker.modules[i].init, lr);
    c->x[30] = lr;
    c->pc = lr;
}
/* each module's destructors (its DT_FINI), in load order */
static void fini(CPU* c, u64 pc) {
    u64 lr = c->x[30];
    u32 i;
    (void)pc;
    for (i = 1; i < g_linker.count; i++) run(c, g_linker.modules[i].fini, lr);
    c->x[30] = lr;
    c->pc = lr;
}

void linker_init(void) {
    Linker* l = &g_linker;
    char error[512];
    static const GuestFn handlers[3] = {ready, init, fini};
    u32 i;
    if (!g_mod || g_arm_mod) return;
    if (l->links) {
        for (i = 0; i < 3; i++) __atomic_store_n(&g_disp[(callback(i) - g_code_lo) / 4], handlers[i], __ATOMIC_RELEASE);
        return;
    }
    /* the game's linker runs: compared with the runtime's, where its last call can be watched */
    if (nce_on()) return;
    if (!l->start) {
        LOG("linker: not compared with the game's own linker: the game has no recompiled nn::init::Start (%s or %s)\n",
            linker_start[0], linker_start[1]);
        return;
    }
    if (!hook_routed(l->start_name)) {
        LOG("linker: not compared with the game's own linker: the module does not route %s. Recompile the game for "
            "it\n",
            l->start_name);
        return;
    }
    for (i = 0; i < g_mod->nmods; i++)
        if ((u64)g_mod->mods[i].data_off + g_mod->mods[i].data_size > g_mod->mods[i].size) return;
    modules_make(1);
    if (snx_relocate(l->modules, l->count, &l->counts, error, sizeof error)) {
        LOG("linker: not compared with the game's own linker: %s\n", error);
        modules_free();
        return;
    }
    l->original = g_disp[(l->start - g_code_lo) / 4];
    __atomic_store_n(&g_disp[(l->start - g_code_lo) / 4], start, __ATOMIC_RELEASE);
}

u64 linker_entry(u64 entry) { return g_linker.links ? g_linker.start : entry; }

int linker_main(u64 args[4]) {
    const SnxModInfo* rtld;
    if (!g_linker.links) return 0;
    rtld = &g_mod->mods[0];
    args[0] = rtld->base + rtld->size; /* the argument's address: right behind the rtld, as its own code gives it */
    args[1] = callback(0);
    args[2] = callback(1);
    args[3] = callback(2);
    return 1;
}

/* the modules' tables were read whole when the runtime linked them, so a lookup has no error left to say */
static int find(u32 first, const char* name, u64* address) {
    char error[512];
    SnxU64 found = 0;
    if (snx_relocate_find(g_linker.modules, g_linker.count, first, name, &found, error, sizeof error) != 1) return 0;
    *address = found;
    return 1;
}

int linker_find(const char* name, u64* address) { return g_linker.links && find(0, name, address); }

int linker_find_after(u64 caller, const char* name, u64* address) {
    u32 i;
    if (!g_linker.links) return 0;
    for (i = 0; i < g_linker.count; i++)
        if (caller - g_linker.modules[i].base < g_linker.modules[i].size) return find(i + 1, name, address);
    return 0;
}
