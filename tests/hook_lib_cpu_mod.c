/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The synthetic hook modules of tests/hook_lib_cpu.c, of no game, with CPU hooks, built against
   nn/snx_hook_module.h alone. HX_CASE picks the case. 1 is hx_cpu, four CPU hooks on the test's game: one that
   leaves the function's own code out, one around it, one the game wasn't recompiled for, one that records an upscale.
   2 is hx_cpu_title, the same for another title. 3 is hx_cpu_nobuild, CPU hooks without a build ID. 4 is hx_cpu_odd,
   a hook at an offset that isn't 4-aligned. 5 is hx_cpu_full, one hook given by the game's whole build ID, where
   hx_cpu gives its first 8 bytes */
#include <string.h>
#include "nx/nn/snx_hook_module.h"

#ifndef HX_CASE
#error HX_CASE picks the case
#endif

#if HX_CASE == 2
#define HX_NAME  "hx_cpu_title"
#define HX_TITLE 0x0100000000def000ull
#elif HX_CASE == 3
#define HX_NAME "hx_cpu_nobuild"
#elif HX_CASE == 4
#define HX_NAME "hx_cpu_odd"
#elif HX_CASE == 5
#define HX_NAME "hx_cpu_full"
#else
#define HX_NAME "hx_cpu"
#endif
#ifndef HX_TITLE
#define HX_TITLE 0x0100000000abc000ull
#endif

#if HX_CASE != 5
/* the function's own code left out: x0 = 2 * x0 + the word at x1, and 0x5a written past it */
static void skip(const SnxHookHost* host, SnxHookCpu* cpu) {
    uint32_t word = 0;
    const uint8_t mark = 0x5a;
    if (!host->read(cpu, host->reg(cpu, 1), &word, sizeof word)) return;
    host->write(cpu, host->reg(cpu, 1) + 4, &mark, 1);
    host->set_reg(cpu, 0, 2 * host->reg(cpu, 0) + word);
}

/* around the function's own code: its argument one more, its result three times, and a second run that does nothing.
   The entry and main's base go in x2 and x3, v1's first byte in x4 (v1 set to 7s after) */
static void around(const SnxHookHost* host, SnxHookCpu* cpu) {
    uint8_t v[16];
    host->set_reg(cpu, 0, host->reg(cpu, 0) + 1);
    host->run_original(cpu);
    host->run_original(cpu);
    host->set_reg(cpu, 0, 3 * host->reg(cpu, 0));
    host->set_reg(cpu, 2, host->entry(cpu));
    host->set_reg(cpu, 3, host->module_base(NULL));
    host->vreg(cpu, 1, v);
    host->set_reg(cpu, 4, v[0]);
    memset(v, 7, sizeof v);
    host->set_vreg(cpu, 1, v);
}

/* never installed: the test's route list leaves its key out */
static void unrouted(const SnxHookHost* host, SnxHookCpu* cpu) { host->set_reg(cpu, 0, 0xdead); }

/* an upscale of the textures at x1 and x2 recorded into the command buffer at x0, its answer in x0 */
static void upscale(const SnxHookHost* host, SnxHookCpu* cpu) {
    SnxHookRecordedUpscale u = {0};
    u.size = sizeof u;
    u.color.texture = host->reg(cpu, 1);
    u.output.texture = host->reg(cpu, 2);
    u.output.view = host->reg(cpu, 3);
    u.render[0] = 64, u.render[1] = 36;
    u.out[0] = 128, u.out[1] = 72;
    u.out_base[0] = 2, u.out_base[1] = 3;
    u.pre_exposure = 1;
    /* the last bit is no flag of the ABI: the runtime drops it */
    u.flags = SNX_HOOK_UPSCALE_RANGE_GIVEN | SNX_HOOK_UPSCALE_SPATIAL_OK | 0x40000000u;
    host->set_reg(cpu, 0, (uint64_t)host->record_upscale(cpu, host->reg(cpu, 0), &u));
}

static const SnxHookCpuHook k_hooks[] = {
#if HX_CASE == 4
    {0x102, "odd", skip},
#else
    {0x100, "skip", skip},
#endif
    {0x200, "around", around},
    {0x300, "unrouted", unrouted},
    {0x400, "upscale", upscale},
};
#else
/* hx_cpu_full's: 0xf011 in x0, the function's own code left out */
static void full(const SnxHookHost* host, SnxHookCpu* cpu) { host->set_reg(cpu, 0, 0xf011); }

static const SnxHookCpuHook k_hooks[] = {{0x500, "full", full}};
#endif

static const SnxHookModule k_module = {.abi_major = SNX_HOOK_ABI_MAJOR,
                                       .abi_minor = SNX_HOOK_ABI_MINOR,
                                       .name = HX_NAME,
                                       .what = "a test module of no game, with CPU hooks",
                                       .title = HX_TITLE,
#if HX_CASE == 5
                                       .build_id = "0123456789abcdef0123456789abcdef01234567",
#elif HX_CASE != 3
                                       .build_id = "0123456789ABCDEF",
#endif
                                       .cpu_hooks = k_hooks,
                                       .ncpu_hooks = sizeof k_hooks / sizeof k_hooks[0]};

SNX_HOOK_EXPORT const SnxHookModule* snx_hook_module(const SnxHookHost* host) {
    return host->abi_major == SNX_HOOK_ABI_MAJOR && host->size >= sizeof(SnxHookHost) ? &k_module : NULL;
}
