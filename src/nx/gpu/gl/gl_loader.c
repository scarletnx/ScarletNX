/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's loader (gl_layer.h): with gl_layer=1, the slot of every egl and _gl function of the game's GL driver
   (the module that exports eglGetProcAddress) takes the layer's handler (gpu/gl/gl.hook), or the one that says the
   function's name once and returns 0. The driver's own code never runs: its eglGetProcAddress is the layer's, which
   answers a name with the address of the driver's export of it, whose slot holds the layer's handler. The exports are
   kept sorted by name for that lookup: their addresses and the guest addresses of their names, in one block */
#include "nx/gpu/gl/gl_layer.h"
#include "gpu/gl/gl.hook.h"
#include "nx/hle/hook/hook.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/memory/mem.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/guest_jit.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/nce/nce.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include <stdlib.h>
#include <string.h>

#include "gpu/gl/gl.hook.inc"

#define GL_SAID 1024 /* a power of two: the functions without a handler said once each, by address */

/* an export of the driver: its address, and its name's in guest memory (the module's string table) */
typedef struct {
    u64 addr, name;
} GlExport;

static struct {
    PMutex lock; /* said */
    GlExport* exports;
    u32 count;
    u64 said[GL_SAID];
} g_gl_loader = {.lock = PMUTEX_INIT};

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

/* a function of the driver the layer has no handler for: its name said once, 0 returned */
static void missing(CPU* RESTRICT c, u64 entry) {
    u32 i = (u32)((entry >> 2) * 2654435761u) & (GL_SAID - 1), n;
    int say = 0;
    pm_lock(&g_gl_loader.lock);
    for (n = 0; n < GL_SAID && g_gl_loader.said[i] && g_gl_loader.said[i] != entry; n++) i = (i + 1) & (GL_SAID - 1);
    if (n < GL_SAID && !g_gl_loader.said[i]) g_gl_loader.said[i] = entry, say = 1;
    pm_unlock(&g_gl_loader.lock);
    if (say) {
        const char* name = "?";
        for (u32 k = 0; k < g_gl_loader.count; k++)
            if (g_gl_loader.exports[k].addr == entry) name = name_of(&g_gl_loader.exports[k]);
        WARN("gl layer: the game called %s, which the layer doesn't implement yet: it returns 0\n",
             name[0] == '_' ? name + 1 : name);
    }
    c->x[0] = 0;
    memset(&c->v[0], 0, sizeof c->v[0]); /* a float result: 0 too */
    c->pc = c->x[30];
}

/* the export at addr takes fn in its slot */
static void take(u64 addr, GuestFn fn) {
    GuestFn* slot = &g_disp[(addr - g_code_lo) / 4];
    __atomic_store_n(slot, fn, __ATOMIC_RELEASE);
    nce_slot_taken(addr, "gl layer");
    jit_slot_taken(addr, fn);
}

void gl_loader_init(void) {
    const SnxModInfo* driver = NULL;
    const char* name;
    u64 getproc, addr;
    u32 versions[HOOK_KEYS] = {0}, implemented = 0, i, n = 0;
    if (!g_settings.gl_layer) return;
    if (g_arm_mod) {
        WARN("gl layer: gl_layer=%d, but the game is 32-bit code, which the layer doesn't take: its own GL driver "
             "draws\n",
             g_settings.gl_layer);
        return;
    }
    getproc = kernel_find_symbol_exact("eglGetProcAddress");
    for (i = 0; getproc && i < g_mod->nmods; i++)
        if (getproc - g_mod->mods[i].base < g_mod->mods[i].size) driver = &g_mod->mods[i];
    if (!driver || getproc < g_code_lo || getproc >= g_code_hi) {
        WARN("gl layer: gl_layer=%d, but no module of the game exports eglGetProcAddress in its code: it has no GL "
             "driver to take\n",
             g_settings.gl_layer);
        return;
    }
    if (!hook_routed("eglInitialize"))
        fatal("gl layer: gl_layer=%d needs the game recompiled with this runtime's route list, which sends its direct "
              "EGL calls to the layer: recompile the game, or set gl_layer=0",
              g_settings.gl_layer);
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
        take(addr, k >= 0 ? gl_fn[k] : missing);
        implemented += k >= 0;
    }
    qsort(g_gl_loader.exports, g_gl_loader.count, sizeof *g_gl_loader.exports, by_name);
    LOG("gl layer: the game's GL driver (%s) doesn't run: the layer implements %u of its %u egl and _gl functions, "
        "and each other one says its name once and returns 0\n",
        driver->name ? driver->name : "?", implemented, g_gl_loader.count);
}
