/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native bridge's start (android/bridge/bridge.c) on a host its native back end cannot make code for, which the linker's
   wrap of snx_recompiler_new stands in for: the start says so and keeps nothing, so the next one, with the backend
   there, is a whole start and not the answer of a bridge that started with no recompiler. On a host below its native
   back end's floor (recompiler/mach/native.h), it says what the host lacks */
#include "android/bridge/bridge.h"
#include "common/cpu/recompiler/mach/native.h"
#include "common/cpu/recompiler/snx_recompiler.h"
#include "common/cpu/recompiler/x64/x64_codegen.h"
#include "sail_x64_emit.h"
#include <assert.h>
#include <string.h>

static struct {
    int calls, refuse; /* the recompilers asked for, and whether the host has none to give */
} g_start;
struct snx_recompiler* __real_snx_recompiler_new(int flags);
struct snx_recompiler* __wrap_snx_recompiler_new(int flags) {
    ++g_start.calls;
    return g_start.refuse ? NULL : __real_snx_recompiler_new(flags);
}

int main(void) {
    char error[256] = "";
    g_start.refuse = 1;
    assert(!bridge_init(NULL, error, sizeof error) && strstr(error, "native back end") && g_start.calls == 1);
    g_start.refuse = 0;
#if defined(__x86_64__)
    if (snx_native_available()) {
        char lacks[256];
        snx_x64_without(SNX_X64_SSE2);
        assert(snx_native_floor(lacks, sizeof lacks) && !bridge_init(NULL, error, sizeof error));
        assert(!strcmp(error, lacks) && !strncmp(error, "cpu: needs SSE2, this host has ", 31));
        snx_x64_without(0);
    }
#endif
    g_start.calls = 0;
    assert(bridge_init(NULL, error, sizeof error) && g_start.calls == 1);
    assert(bridge_init(NULL, error, sizeof error) && g_start.calls == 1); /* started: nothing is made again */
    return 0;
}
