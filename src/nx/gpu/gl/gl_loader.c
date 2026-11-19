/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's loader (gl_layer.h): the slot of every egl and _gl function of the game's GL driver (the module that
   exports eglGetProcAddress) takes the layer's handler (gpu/gl/gl.hook), or the one that stops the game naming the
   function, as nvn_loader.c does for NVN. The driver's own code never runs: its eglGetProcAddress is the layer's, which
   answers a name with the address of the driver's export of it, whose slot holds the layer's handler. The exports are
   kept sorted by name for that lookup: their addresses and the guest addresses of their names, in one block. A 32-bit
   game keeps its driver, whose submissions the GPU's front end reads */
#include "nx/gpu/gl/gl_layer.h"
#include "gpu/gl/gl.hook.h"
#include "nx/hle/hook/hook.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/memory/mem.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/guest_jit.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/nce/nce.h"
#include "nx/runtime/platform/log.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "gpu/gl/gl.hook.inc"

/* an export of the driver: its address, and its name's in guest memory (the module's string table) */
typedef struct {
    u64 addr, name;
} GlExport;

static struct {
    GlExport* exports;
    u32 count;
} g_gl_loader;

static const char* name_of(const GlExport* e) { return (const char*)GP(e->name); }
static int by_name(const void* a, const void* b) { return strcmp(name_of(a), name_of(b)); }

u64 gl_loader_lookup(const char* name) {
    char sym[160];
    u32 lo = 0, hi = g_gl_loader.count;
    /* GL's functions are exported with a '_' before their name, EGL's as they are */
    snprintf(sym, sizeof sym, "%s%s", strncmp(name, "gl", 2) ? "" : "_", name);
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        int order = strcmp(name_of(&g_gl_loader.exports[mid]), sym);
        if (!order) return g_gl_loader.exports[mid].addr;
        if (order < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

/* a function of the driver the layer has no handler for: the game stops, naming it. Going on with a made-up result
   would only move the failure to a later call that no longer says what caused it */
static void unimplemented(CPU* RESTRICT c, u64 entry) {
    (void)c;
    for (u32 k = 0; k < g_gl_loader.count; k++)
        if (g_gl_loader.exports[k].addr == entry) {
            const char* name = name_of(&g_gl_loader.exports[k]);
            fatal("gl layer: the game called %s, which the GL layer doesn't implement yet",
                  name[0] == '_' ? name + 1 : name);
        }
    fatal("gl layer: the game called the GL driver's function at %llx, which the GL layer doesn't implement yet",
          (unsigned long long)entry);
}

/* the export at addr takes fn in its slot */
static void take(u64 addr, GuestFn fn) {
    atomic_store_explicit(&g_disp[(addr - g_code_lo) / 4], fn, memory_order_release);
    nce_slot_taken(addr, "gl layer");
    jit_slot_taken(addr, fn);
}

void gl_loader_init(void) {
    const SnxModInfo* driver = NULL;
    const char* name;
    u64 getproc, addr;
    u32 versions[HOOK_KEYS] = {0}, implemented = 0, i, n = 0;
    getproc = kernel_find_symbol_exact("eglGetProcAddress");
    if (!getproc) return; /* no GL driver: the game draws through NVN or the GPU itself */
    if (g_arm_mod) {
        WARN("gl layer: the game is 32-bit code, which the layer doesn't take: its own GL driver draws\n");
        return;
    }
    for (i = 0; i < g_mod->nmods; i++)
        if (getproc - g_mod->mods[i].base < g_mod->mods[i].size) driver = &g_mod->mods[i];
    if (!driver || getproc < g_code_lo || getproc >= g_code_hi) {
        WARN("gl layer: eglGetProcAddress at %llx is in no module's code: the game's GL driver draws\n",
             (unsigned long long)getproc);
        return;
    }
    if (!hook_routed("eglInitialize"))
        fatal(
            "gl layer: the game's GL driver needs the game recompiled with this runtime's route list, which sends its "
            "direct EGL calls to the layer: recompile the game");
    for (i = 0; kernel_symbol(i, &addr, &name); i++)
        if (name && addr - driver->base < driver->size && (!strncmp(name, "egl", 3) || !strncmp(name, "_gl", 3))) n++;
    g_gl_loader.exports = calloc(n ? n : 1, sizeof *g_gl_loader.exports);
    if (!g_gl_loader.exports) fatal("gl layer: no memory for the %u functions of the game's GL driver", n);
    for (i = 0; kernel_symbol(i, &addr, &name); i++) {
        int first, k;
        if (!name || addr - driver->base >= driver->size || (strncmp(name, "egl", 3) && strncmp(name, "_gl", 3)))
            continue;
        g_gl_loader.exports[g_gl_loader.count].addr = addr;
        g_gl_loader.exports[g_gl_loader.count].name = (u64)((const u8*)name - g_mem);
        g_gl_loader.count++;
        k = hook_select(&gl_table, name, versions, &first);
        take(addr, k >= 0 ? gl_fn[k] : unimplemented);
        implemented += k >= 0;
    }
    qsort(g_gl_loader.exports, g_gl_loader.count, sizeof *g_gl_loader.exports, by_name);
    gl_program_calls_init();
    gl_object_calls_init();
    gl_texture_calls_init();
    gl_framebuffer_calls_init();
    gl_sync_calls_init();
    LOG("gl layer: the game's GL driver (%s) doesn't run: the layer implements %u of its %u egl and _gl functions, "
        "and a call of any other one stops the game, naming it\n",
        driver->name ? driver->name : "?", implemented, g_gl_loader.count);
}
