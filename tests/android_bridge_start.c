/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native bridge's start (android/bridge/bridge.c) on a host the LLVM backend cannot target, which the linker's
   wrap of snx_recompiler_new stands in for: the start says so and keeps nothing, so the next one, with the backend
   there, is a whole start and not the answer of a bridge that started with no recompiler */
#include "android/bridge/bridge.h"
#include "common/cpu/recompiler/snx_recompiler.h"
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
    assert(!bridge_init(NULL, error, sizeof error) && strstr(error, "LLVM backend") && g_start.calls == 1);
    g_start.refuse = 0;
    assert(bridge_init(NULL, error, sizeof error) && g_start.calls == 2);
    assert(bridge_init(NULL, error, sizeof error) && g_start.calls == 2); /* started: nothing is made again */
    return 0;
}
