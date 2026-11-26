/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The profiler's macros in a build without its client (runtime/platform/tracy.h's stubs): none of them evaluates an
   argument, so a call a caller wrote into one (a clock read for a plot, a lookup for a zone's text) never runs. And
   nvn_queue_context_set (gpu/nvn/nvn_queue.h), whose arguments are read only with a client */
#undef SNX_TRACY
#include "nx/gpu/nvn/nvn_queue.h"
#include "common/utilities/tracy.h"
#include <stdio.h>

static struct {
    unsigned long long evaluated; /* the arguments a macro evaluated */
    int set;                      /* nvn_queue_context_set's calls */
} g_tracy_off;

static unsigned long long touch(void) { return ++g_tracy_off.evaluated; }
static const char* name(void) { return touch() ? "touched" : ""; }

/* the context setter this test stands in for, counting what reaches it */
void(nvn_queue_context_set)(const char* fmt, ...) {
    (void)fmt;
    ++g_tracy_off.set;
}
const char* nvn_queue_context(void) { return ""; }

int main(void) {
    int failures = 0;
    void* p = &g_tracy_off;
    SNX_ZONE(z, name());
    SNX_ZONE_C(zc, name(), touch());
    SNX_ZONE_CS(zcs, name(), touch());
    SNX_ZONE_TRANSIENT(zt, name());
    SNX_ZONE_TRANSIENT_CS(ztcs, name(), touch());
    SNX_ZONE_T(ztext, name(), "%llu", touch());
    SNX_ZONE_V(zv, name(), touch());
    SNX_ZONE_TV(ztv, name(), touch(), "%llu", touch());
    SNX_ZONE_COLOR(z, touch());
    SNX_ZONE_TEXT(z, "%llu", touch());
    SNX_ZONE_NAME(z, name(), touch());
    SNX_ZONE_NAME_F(z, "%llu", touch());
    SNX_ZONE_VALUE(z, touch());
    SNX_MSG(name(), touch());
    SNX_LOG(name(), touch(), touch(), touch(), touch());
    SNX_MSG_CS(name(), touch(), touch());
    SNX_MEM_ALLOC(p, touch(), name());
    SNX_MEM_FREE(p, name());
    SNX_MEM_ALLOC_D(p, touch(), name());
    SNX_MEM_FREE_D(p, name());
    SNX_MEM_DISCARD(name());
    SNX_FRAME_MARK_NAMED(name());
    SNX_FRAME_SCOPED(f, name());
    SNX_SECTION_SCOPED_C(sc, name(), touch());
    SNX_SECTION_SCOPED(s, name());
    SNX_LOCK_MARK(touch());
    SNX_PLOT(name(), (double)touch());
    SNX_PLOT_CFG(name(), (double)touch(), touch(), touch(), touch(), touch());
    SNX_ZONE_END(z);
    if (g_tracy_off.evaluated) {
        fprintf(stderr, "FAIL the stubs evaluated %llu arguments, where they evaluate none\n", g_tracy_off.evaluated);
        failures++;
    } else
        puts("PASS the stubs evaluate none of their arguments");
    g_tracy_off.evaluated = 0;
    nvn_queue_context_set("%llu %s", touch(), name());
    if (g_tracy_off.evaluated || g_tracy_off.set) {
        fprintf(stderr,
                "FAIL nvn_queue_context_set without a client: %llu arguments evaluated and %d calls, where it "
                "makes none\n",
                g_tracy_off.evaluated, g_tracy_off.set);
        failures++;
    } else
        puts("PASS nvn_queue_context_set reads its arguments only with a client");
    return failures != 0;
}
