/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game module that is its descriptor alone, for module_abi.c: the ABI SNX_TEST_MODULE_ABI gives, and one program,
   main, which the game's data folder doesn't hold, so a runtime that takes the module stops at loading the game.
   Built with SNX_TEST_MODULE_ARM, a 32-bit program's module */
#include "nx/loader/module_abi.h"
#include <stdlib.h>

static struct {
    SnxModInfo program;
    SnxModule module;
    SnxArmModule arm;
} g_module_abi_fixture;

static u32 fixture_abi(void) {
    const char* abi = getenv("SNX_TEST_MODULE_ABI");
    return abi ? (u32)strtoul(abi, NULL, 0) : 0;
}

#ifdef SNX_TEST_MODULE_ARM
SNX_EXPORT const SnxArmModule* snx_module_arm(void) {
    g_module_abi_fixture.program.name = "main";
    g_module_abi_fixture.arm.abi = fixture_abi();
    g_module_abi_fixture.arm.nmods = 1;
    g_module_abi_fixture.arm.mods = &g_module_abi_fixture.program;
    g_module_abi_fixture.arm.title = "ABI fixture";
    return &g_module_abi_fixture.arm;
}
#else
SNX_EXPORT const SnxModule* snx_module(void) {
    g_module_abi_fixture.program.name = "main";
    g_module_abi_fixture.module.abi = fixture_abi();
    g_module_abi_fixture.module.nmods = 1;
    g_module_abi_fixture.module.mods = &g_module_abi_fixture.program;
    g_module_abi_fixture.module.title = "ABI fixture";
    return &g_module_abi_fixture.module;
}
#endif
