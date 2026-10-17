/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime JIT's NX side (jit.h): the game's code ranges and the module's entries handed to the service, and the
   hints of earlier runs queued for it. There is no interpreter behind it: a JIT that can't start leaves only the code
   the recompiler found, and the first pc it missed ends the game with the reason (rtapi.c's rt_miss) */
#include "nx/runtime/guest_jit.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/rtapi.h"
#include "nx/gpu/nvn_trace.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "common/cpu/recompiler/llvm/jit_service.h"
#include <stdlib.h>
#include <string.h>

static SnxJitService* g_jit;
static char g_jit_error[512];      /* why there is no JIT, for the fatal error of the first miss */
static char g_jit_cache[1400];     /* <Game>.jitcache in the user's cache folder: the units of earlier runs */
static SnxCodeImage* g_jit_images; /* the modules' text in guest memory, for as long as the service lives */

/* on a compile thread */
static void jit_log(const SnxJitReport* r) {
    if (r->error)
        WARN_LIMIT(32, "jit: %llx and %lu more pcs could not be compiled: %s\n", (unsigned long long)r->pc,
                   (unsigned long)(r->pcs - 1), r->error);
    else
        LOGV("jit: %llx and %lu more pcs compiled, %lu functions, %lu instructions, %lu KiB, %.1f ms\n",
             (unsigned long long)r->pc, (unsigned long)(r->pcs - 1), (unsigned long)r->functions,
             (unsigned long)r->instructions, (unsigned long)(r->bytes >> 10), r->seconds * 1000.0);
}

void jit_init(void) {
    SnxJitConfig config;
    SnxJitStats stats;
    char error[512];
    u64* hints;
    size_t hint_count, warm = 0;
    u32 i;
    g_jit_images = (SnxCodeImage*)calloc(g_mod->nmods ? g_mod->nmods : 1, sizeof(*g_jit_images));
    if (!g_jit_images) {
        snprintf(g_jit_error, sizeof(g_jit_error), "out of memory");
        WARN("jit: out of memory, so only the code the recompiler found can run\n");
        return;
    }
    for (i = 0; i < g_mod->nmods; i++) {
        const SnxModInfo* m = &g_mod->mods[i];
        g_jit_images[i].image = GP(m->base);
        g_jit_images[i].image_size = (size_t)m->size;
        g_jit_images[i].base = m->base;
        g_jit_images[i].text_off = m->text_off;
        g_jit_images[i].text_size = m->text_size;
    }
    memset(&config, 0, sizeof(config));
    config.images = g_jit_images;
    config.image_count = g_mod->nmods;
    config.dispatch = g_disp;
    config.code_lo = g_code_lo;
    config.code_hi = g_code_hi;
    config.aot_addresses = g_mod->entry_addr;
    config.aot_functions = g_mod->entry_fn;
    config.aot_count = g_mod->nentries;
    /* with SNX_NVN_TRACE, wrappers sit in dispatch slots, which only calls through the table see */
    config.hook_direct_calls = nvn_trace_active();
    config.threads = (unsigned)g_opt_jit_threads;
    config.report = jit_log;
    snprintf(g_jit_cache, sizeof(g_jit_cache), "%s%s.jitcache", g_cachedir, g_exename);
    config.cache_path = g_jit_cache;
    g_jit = snx_jit_service_new(&config, error, sizeof(error));
    if (!g_jit) {
        snprintf(g_jit_error, sizeof(g_jit_error), "%s", error);
        WARN("jit: cannot start (%s), so only the code the recompiler found can run\n", error);
        free(g_jit_images);
        g_jit_images = NULL;
        return;
    }
    /* warm start: what earlier runs missed is compiled before the game reaches it, if the JIT has time before then */
    hints = hints_copy(&hint_count);
    if (hints) warm = snx_jit_warm(g_jit, hints, hint_count);
    free(hints);
    snx_jit_stats(g_jit, &stats);
    LOG("jit: guest code the recompiler missed is compiled when a thread reaches it, on %u threads. %lu pcs of earlier "
        "runs queued\n",
        stats.threads, (unsigned long)warm);
}

GuestFn jit_resolve(u64 pc) { return g_jit ? snx_jit_resolve(g_jit, pc) : NULL; }

GuestFn jit_wait(u64 pc) { return g_jit ? snx_jit_wait(g_jit, pc) : NULL; }

const char* jit_failure(void) { return g_jit ? "the JIT could not compile it" : g_jit_error; }

int jit_code_added(u64 base, u64 text_size, u64 size) {
    SnxCodeImage image;
    if (!g_jit) return -1;
    memset(&image, 0, sizeof(image));
    image.image = GP(base);
    image.image_size = (size_t)size;
    image.base = base;
    image.text_size = text_size;
    return snx_jit_add_image(g_jit, &image);
}

void jit_code_removed(u64 base) {
    if (g_jit) snx_jit_remove_image(g_jit, base);
}

void jit_report(void) {
    SnxJitStats s;
    if (!g_jit) return;
    snx_jit_stats(g_jit, &s);
    LOG("jit: %lu batches compiled %lu pcs (%lu failed, %lu still queued), %lu functions, %lu instructions, %lu KiB of "
        "code, %.2f s compiling, %lu units loaded from %s\n",
        s.batches, s.compiled, s.failures, s.queued, s.functions, s.instructions, (unsigned long)(s.bytes >> 10),
        s.seconds, s.cached, g_jit_cache);
}
