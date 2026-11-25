/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* hook_install.c's module cases: one hook module, named after tests/hook/install.hook's plain family, which applies
   when main's code at +0x40 holds the four bytes it checks. No library's module (module_lib.h) */
#include "nx/hle/hook/module.h"
#include "nx/hle/hook/module_lib.h"
#include "nx/loader/hook_routes.h"

static const HookModuleCode code[] = {{NULL, 0x40, "de ad be ef"}};
static const HookModule plain = {"plain", "the test spec's plain family as a module", 0, NULL, NULL, code, 1};
const HookModule* const g_hook_modules[] = {&plain, NULL};
void hook_module_libs_load(int (*off)(const char* name)) { (void)off; }
u32 hook_lib_cpu_count(void) { return 0; }
HookLibCpu* hook_lib_cpu(u32 i) { return (void)i, NULL; }
void hook_lib_cpu_run(u32 i, CPU* c, u64 entry) { (void)i, (void)c, (void)entry; }
int snx_hook_build_key(const unsigned char build_id[32], SnxU64 offset, char* out, size_t size) {
    return (void)build_id, (void)offset, (void)out, (void)size, 0;
}
