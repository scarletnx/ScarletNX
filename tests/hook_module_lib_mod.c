/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The synthetic hook modules of tests/hook_module_lib.c, of no game: one source built into a library per case, as a
   module built outside ScarletNX is, against hle/hook/snx_hook_module.h alone. HX_CASE picks the case. 1 is the module
   that applies, 2 the same named hx_off (hooks_off names it, so it is never loaded), 3 another ABI major, 4 a newer ABI
   minor, 5 no entry point, 6 an entry point that refuses the runtime, 7 a module named otherwise than its file, 8 a
   module without a CPU hook */
#include "nx/hle/hook/snx_hook_module.h"

#ifndef HX_CASE
#error HX_CASE picks the case
#endif

#if HX_CASE == 2
#define HX_NAME "hx_off"
/* hooks_off names it: the test checks that the process never has it loaded */
#elif HX_CASE == 3
#define HX_NAME "hx_major"
#elif HX_CASE == 4
#define HX_NAME "hx_minor"
#elif HX_CASE == 6
#define HX_NAME "hx_null"
#elif HX_CASE == 7
#define HX_NAME "hx_other"
#elif HX_CASE == 8
#define HX_NAME "hx_empty"
#else
#define HX_NAME "hx_good"
#endif

/* the function's own code, and its result one more */
static void around(const SnxHookHost* host, SnxHookCpu* cpu) {
    host->run_original(cpu);
    host->set_reg(cpu, 0, host->reg(cpu, 0) + 1);
}

static const SnxHookCpuHook k_hooks[] = {{0x80, "around", around}};
static const SnxHookCode k_code[] = {{NULL, 0x40, "de ad ?? ef"}};

static const SnxHookModule k_module = {
#if HX_CASE == 3
    .abi_major = SNX_HOOK_ABI_MAJOR + 1,
    .abi_minor = 0,
#elif HX_CASE == 4
    .abi_major = SNX_HOOK_ABI_MAJOR,
    .abi_minor = SNX_HOOK_ABI_MINOR + 1,
#else
    .abi_major = SNX_HOOK_ABI_MAJOR,
    .abi_minor = SNX_HOOK_ABI_MINOR,
#endif
    .name = HX_NAME,
    .what = "a test module of no game",
    .title = 0x0100000000abc000ull,
    .build_id = "0123456789abcdef",
    .code = k_code,
    .ncode = 1,
    .cpu_hooks = k_hooks,
#if HX_CASE == 8
    .ncpu_hooks = 0};
#else
    .ncpu_hooks = 1};
#endif

#if HX_CASE == 5
/* an export, but not the entry point */
SNX_HOOK_EXPORT const SnxHookModule* snx_hook_module_old(void);
SNX_HOOK_EXPORT const SnxHookModule* snx_hook_module_old(void) { return &k_module; }
#else
SNX_HOOK_EXPORT const SnxHookModule* snx_hook_module(const SnxHookHost* host) {
#if HX_CASE == 6
    /* it needs a minor no runtime has */
    if (host->abi_minor < 1000) {
        host->log(SNX_HOOK_LOG_WARN, "hx_null: the runtime's hook ABI %u.%u is older than it needs", host->abi_major,
                  host->abi_minor);
        return NULL;
    }
    return &k_module;
#else
    /* a module reads what its minor has: the host's table is at least this long */
    if (host->abi_major != SNX_HOOK_ABI_MAJOR || host->size < sizeof(SnxHookHost)) return NULL;
    return &k_module;
#endif
}
#endif
