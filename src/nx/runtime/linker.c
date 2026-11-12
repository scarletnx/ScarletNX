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

typedef struct {
    SnxRelocModule* modules; /* one block: the modules, then the copy of each one's data */
    u32 count;
    SnxRelocCounts counts;
    GuestFn original; /* what nn::init::Start's dispatch slot held */
    u64 entry;
    int done;
} Linker;
static Linker g_linker;

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
    if (l->counts.absent || l->counts.unresolved)
        LOG("linker: nothing defines the symbol of %llu words that are weak (0) and of %llu that are not (as "
            "loaded)%s%s\n",
            (unsigned long long)l->counts.absent, (unsigned long long)l->counts.unresolved,
            l->counts.first_unresolved ? ", the first " : "",
            l->counts.first_unresolved ? l->counts.first_unresolved : "");
    if (!diff.differ && (l->counts.outside || l->counts.other))
        LOG("linker: %llu relocations of a type the runtime's linker has no case for and %llu outside their module's "
            "data were not compared\n",
            (unsigned long long)l->counts.other, (unsigned long long)l->counts.outside);
    free(l->modules);
    l->modules = NULL;
}

/* nn::init::Start's slot until its first call: the game's linker has relocated every module by then, and nothing
   else of the game has run */
static void start(CPU* c, u64 entry) {
    GuestFn original = g_linker.original;
    if (!__atomic_exchange_n(&g_linker.done, 1, __ATOMIC_ACQ_REL)) {
        __atomic_store_n(&g_disp[(g_linker.entry - g_code_lo) / 4], original, __ATOMIC_RELEASE);
        compare();
    }
    original(c, entry);
}

void linker_init(void) {
    Linker* l = &g_linker;
    const char* name = NULL;
    char error[512];
    unsigned char* copy;
    size_t bytes = 0;
    u64 entry = 0;
    u32 i;
    if (!g_mod || g_arm_mod) return; /* a 32-bit game's modules are ELF32 */
    if (nce_on()) {
        LOG("linker: not compared with the game's own linker under NCE\n");
        return;
    }
    for (i = 0; i < sizeof linker_start / sizeof linker_start[0] && !entry; i++) {
        name = linker_start[i];
        entry = kernel_find_symbol_exact(name);
    }
    if (!entry || entry < g_code_lo || entry >= g_code_hi || (entry & 3) || !g_disp[(entry - g_code_lo) / 4]) {
        LOG("linker: not compared with the game's own linker: the game has no recompiled nn::init::Start (%s or %s)\n",
            linker_start[0], linker_start[1]);
        return;
    }
    if (!hook_routed(name)) {
        LOG("linker: not compared with the game's own linker: the module does not route %s. Recompile the game for "
            "it\n",
            name);
        return;
    }
    for (i = 0; i < g_mod->nmods; i++) {
        const SnxModInfo* mi = &g_mod->mods[i];
        if ((u64)mi->data_off + mi->data_size > mi->size) return; /* loader_load refused such a module already */
        bytes += mi->data_size;
    }
    l->count = g_mod->nmods;
    l->modules = (SnxRelocModule*)snx_alloc(l->count * sizeof(*l->modules) + bytes);
    copy = (unsigned char*)(l->modules + l->count);
    for (i = 0; i < l->count; i++) {
        const SnxModInfo* mi = &g_mod->mods[i];
        SnxRelocModule* m = &l->modules[i];
        m->name = mi->name;
        m->image = GP(mi->base);
        m->base = mi->base;
        m->size = mi->size;
        m->data = copy;
        m->data_off = mi->data_off;
        m->data_size = mi->data_size;
        m->lazy = 0;
        memcpy(copy, GP(mi->base + mi->data_off), mi->data_size);
        copy += mi->data_size;
    }
    if (snx_relocate(l->modules, l->count, &l->counts, error, sizeof error)) {
        LOG("linker: not compared with the game's own linker: %s\n", error);
        free(l->modules);
        l->modules = NULL;
        return;
    }
    l->entry = entry;
    l->original = g_disp[(entry - g_code_lo) / 4];
    __atomic_store_n(&g_disp[(entry - g_code_lo) / 4], start, __ATOMIC_RELEASE);
}
