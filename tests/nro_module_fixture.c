/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An NRO's own AOT module as the recompiler makes one (module_llvm.c), written by hand: one module, an NRO, of
   0x1000 bytes with the module ID 0x42 thirty-two times, and two functions, 0x100 and 0x200 into it, each 0x40 bytes
   of guest code, which leave their own guest address in x0. snx_module fills the entries again at each call, as the
   recompiler's does. runtime_variants loads it through guest_jit.c: snx_fixture gives it what a copy of the module
   says of itself, the ABI and the address it was made for, before the runtime asks */
#include "nx/loader/module_abi.h"

typedef struct {
    u32 abi;
    u64 base;
} Fixture;
static Fixture g_fixture = {SNX_ABI, 0x1000};

static void first(CPU* c, u64 pc) {
    (void)pc;
    c->x[0] = g_fixture.base + 0x100;
}
static void second(CPU* c, u64 pc) {
    (void)pc;
    c->x[0] = g_fixture.base + 0x200;
}

static SnxModInfo g_info;
static u64 g_entry_addr[2], g_fn_lo[2];
static GuestFn g_entry_fn[2];
static const GuestFn g_fns[2] = {first, second};
static const u32 g_fn_len[2] = {0x40, 0x40};
static u8 g_stale[2];
static SnxModule g_module;

SNX_EXPORT Fixture* snx_fixture(void);
SNX_EXPORT Fixture* snx_fixture(void) { return &g_fixture; }
SNX_EXPORT const SnxModule* snx_module(void);
SNX_EXPORT const SnxModule* snx_module(void) {
    unsigned i;
    g_info.name = "main";
    g_info.base = g_fixture.base;
    g_info.size = 0x1000;
    for (i = 0; i < 32; i++) g_info.build_id[i] = 0x42;
    g_info.text_size = 0x1000;
    g_info.is_nro = 1;
    for (i = 0; i < 2; i++) {
        g_entry_addr[i] = g_fn_lo[i] = g_fixture.base + 0x100 * (i + 1);
        g_entry_fn[i] = g_fns[i];
    }
    g_module.abi = g_fixture.abi;
    g_module.nmods = 1;
    g_module.mods = &g_info;
    g_module.nentries = 2;
    g_module.entry_addr = g_entry_addr;
    g_module.entry_fn = g_entry_fn;
    g_module.code_lo = g_fixture.base;
    g_module.code_hi = g_fixture.base + 0x1000;
    g_module.title = "fixture";
    g_module.nfns = 2;
    g_module.fn = g_fns;
    g_module.fn_lo = g_fn_lo;
    g_module.fn_len = g_fn_len;
    g_module.stale = g_stale;
    return &g_module;
}
