/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/linker.h"
#include "common/utilities/common.h"
#include "nx/hle/hook/hook.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/services/services.h"
#include "nx/loader/relocate.h"
#include "nx/memory/mem.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/nce/nce.h"
#include "nx/runtime/platform/log.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* nn::init::Start as the SDKs export it, with three callbacks of the linker's and with two: what a game's linker
   calls last. The route list names them (hle/hook/hooks.hook), so the module calls them through their slots */
static const char* const linker_start[] = {"_ZN2nn4init5StartEmmPFvvES2_S2_", "_ZN2nn4init5StartEmmPFvvES2_"};
/* a function of nn::ro the module must route for the runtime to answer the library (nn/ro.hook) */
#define LINKER_RO_ROUTED "_ZN2nn2ro12LookupSymbolEPmPKc"
/* the NROs a game has loaded at once at most: as many as an ldr:ro state maps (services_int.h's RO_LIMIT) */
#define LINKER_NROS      64
/* the SDK's count of a module's thread-local destructors no thread has run yet, by the address of its DSO handle,
   which an NRO's header names at 0x60: it does not unload a module that has any */
#define LINKER_TLS_LEFT  "__nnmusl_get_number_uncalled_tls_dtors"

typedef struct {
    /* one block: the game's modules, then while the game's linker is watched each one's copy, else room for the NROs
       the game loads, in the order of their loads */
    SnxRelocModule* modules;
    u32 count;
    u32 loaded;                  /* the NROs behind the game's modules */
    char names[LINKER_NROS][24]; /* theirs, for messages */
    PMutex lock;                 /* the modules past count and loaded, once the runtime linked the game */
    SnxRelocCounts counts;
    u32 named;        /* which of linker_start names it */
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
    l->modules = (SnxRelocModule*)snx_alloc((l->count + (copy ? 0 : LINKER_NROS)) * sizeof(*l->modules) + bytes);
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
        g_linker.named = i;
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
        pm_init(&l->lock);
        l->links = 1;
        LOG("linker: the runtime linked the game's %u modules itself, and its rtld does not run: %llu relative "
            "relocations, %llu with a symbol, %llu PLT slots, each bound\n",
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
    if (!hook_routed(linker_start[l->named])) {
        LOG("linker: not compared with the game's own linker: the module does not route %s. Recompile the game for "
            "it\n",
            linker_start[l->named]);
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
    Linker* l = &g_linker;
    char error[512];
    SnxU64 found = 0;
    int is;
    pm_lock(&l->lock);
    is = snx_relocate_find(l->modules, l->count + l->loaded, first, name, &found, error, sizeof error) == 1;
    pm_unlock(&l->lock);
    if (is) *address = found;
    return is;
}

int linker_find(const char* name, u64* address) { return g_linker.links && find(0, name, address); }

int linker_find_after(u64 caller, const char* name, u64* address) {
    Linker* l = &g_linker;
    u32 i, after = 0;
    if (!l->links) return 0;
    pm_lock(&l->lock);
    for (i = 0; i < l->count + l->loaded && !after; i++)
        if (caller - l->modules[i].base < l->modules[i].size) after = i + 1;
    pm_unlock(&l->lock);
    return after && find(after, name, address);
}

/* ---- the NROs the game loads through nn::ro ---- */

/* which of the modules is the loaded NRO that holds the address, or 0 for none: under the lock */
static u32 nro_holding(u64 address) {
    const Linker* l = &g_linker;
    u32 i;
    for (i = l->count; i < l->count + l->loaded; i++)
        if (address - l->modules[i].base < l->modules[i].size) return i;
    return 0;
}

u32 linker_nro_load(CPU* c, u64 image, u64 size, u64 buffer, u64 buffer_size, int closed, u64* base) {
    Linker* l = &g_linker;
    SnxRelocModule* m;
    SnxRelocCounts counts;
    char error[512];
    u64 lr = c->x[30], total = size + buffer_size, mod0, zero, zero_end, init;
    int r = ldr_own_load(image, size, buffer, buffer_size, base);
    if (r) return (u32)r;
    /* the module's header names its BSS, which the game's buffer is mapped as with whatever it held */
    mod0 = snx_ld_le32(GP(*base + 4));
    if (total < 0x1c || mod0 > total - 0x1c || memcmp(GP(*base + mod0), "MOD0", 4))
        fatal("linker: the NRO the game loaded at %llx (%llu bytes) has no module header (MOD0) at %llx of it, which "
              "the SDK stops the game on",
              (unsigned long long)*base, (unsigned long long)total, (unsigned long long)mod0);
    zero = mod0 + snx_ld_le32(GP(*base + mod0 + 8));
    zero_end = mod0 + snx_ld_le32(GP(*base + mod0 + 12));
    if (zero > zero_end || zero_end > total)
        fatal("linker: the NRO the game loaded at %llx names its BSS at %llx to %llx, outside its %llu bytes",
              (unsigned long long)*base, (unsigned long long)zero, (unsigned long long)zero_end,
              (unsigned long long)total);
    memset(GP(*base + zero), 0, (size_t)(zero_end - zero));
    pm_lock(&l->lock);
    if (l->loaded == LINKER_NROS)
        fatal("linker: the game loaded more than %d NROs at once, all the runtime's linker has room for", LINKER_NROS);
    m = &l->modules[l->count + l->loaded];
    memset(m, 0, sizeof(*m));
    snprintf(l->names[l->loaded], sizeof l->names[l->loaded], "NRO %llx", (unsigned long long)*base);
    m->name = l->names[l->loaded];
    m->image = GP(*base);
    m->base = *base;
    m->size = total;
    m->data_off = snx_ld_le32(GP(*base + 0x30)); /* its data segment, which the BSS follows */
    if (m->data_off > total) m->data_off = total;
    m->data_size = total - m->data_off;
    m->data = GP(*base + m->data_off);
    m->closed = closed;
    if (snx_relocate_added(l->modules, l->count + l->loaded + 1, &counts, error, sizeof error))
        fatal("linker: the NRO the game loaded at %llx does not link: %s", (unsigned long long)*base, error);
    l->loaded++;
    init = m->init;
    pm_unlock(&l->lock);
    LOG("linker: linked the NRO at %llx: %llu relative relocations, %llu with a symbol, %llu PLT slots, each bound; "
        "%llu words of the modules before it took its definitions; nothing defines the symbol of %llu words that "
        "are weak (0) and of %llu that are not (as loaded)%s%s\n",
        (unsigned long long)*base, (unsigned long long)counts.relative, (unsigned long long)counts.symbolic,
        (unsigned long long)counts.slots, (unsigned long long)counts.earlier, (unsigned long long)counts.absent,
        (unsigned long long)counts.unresolved, counts.first_unresolved ? ", the first " : "",
        counts.first_unresolved ? counts.first_unresolved : "");
    if (counts.outside || counts.other)
        LOG("linker: %llu relocations of the NRO at %llx of a type the runtime's linker has no case for and %llu "
            "outside its data were left alone\n",
            (unsigned long long)counts.other, (unsigned long long)*base, (unsigned long long)counts.outside);
    run(c, init, lr); /* its constructors, on the thread that loads it */
    c->x[30] = lr;
    return 0;
}

int linker_nro_unload(CPU* c, u64 inside) {
    Linker* l = &g_linker;
    char error[512];
    u64 lr = c->x[30], base, fini, left = 0, words = 0;
    u32 which, k;
    int closed;
    pm_lock(&l->lock);
    which = nro_holding(inside);
    base = which ? l->modules[which].base : 0;
    fini = which ? l->modules[which].fini : 0;
    pm_unlock(&l->lock);
    if (!which) return 0;
    if (find(0, LINKER_TLS_LEFT, &left)) {
        c->x[0] = base + snx_ld_le32(GP(base + 0x60));
        run(c, left, lr);
        if (c->x[0])
            fatal("linker: the game unloads the NRO at %llx while %llu of its thread-local destructors have not run, "
                  "which the SDK stops the game on",
                  (unsigned long long)base, (unsigned long long)c->x[0]);
    }
    run(c, fini, lr); /* its destructors, while the others still hold its addresses */
    c->x[30] = lr;
    pm_lock(&l->lock);
    which = nro_holding(base);
    if (!which) fatal("linker: the NRO at %llx was unloaded twice at once", (unsigned long long)base);
    closed = l->modules[which].closed;
    if (!closed && snx_relocate_removed(l->modules, l->count + l->loaded, which, &words, error, sizeof error))
        fatal("linker: the NRO at %llx does not unlink: %s", (unsigned long long)base, error);
    for (k = which - l->count; k + 1 < l->loaded; k++) {
        l->modules[l->count + k] = l->modules[l->count + k + 1];
        memcpy(l->names[k], l->names[k + 1], sizeof l->names[k]);
        l->modules[l->count + k].name = l->names[k];
    }
    l->loaded--;
    pm_unlock(&l->lock);
    if (ldr_own_unload(base))
        fatal("linker: the NRO at %llx that the game unloads is not mapped as it was loaded, which the SDK stops "
              "the game on",
              (unsigned long long)base);
    LOG("linker: unlinked the NRO at %llx: %llu words of the other modules that pointed into it are 0, or their "
        "slot's lazy value, again\n",
        (unsigned long long)base, (unsigned long long)words);
    return 1;
}

int linker_nro_find(u64 inside, const char* name, u64* address) {
    Linker* l = &g_linker;
    char error[512];
    SnxU64 found = 0;
    u32 which;
    int is = 0;
    pm_lock(&l->lock);
    which = nro_holding(inside);
    if (which) is = snx_relocate_own(&l->modules[which], name, &found, error, sizeof error) == 1;
    pm_unlock(&l->lock);
    if (is) *address = found;
    return is;
}
