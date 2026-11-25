/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* hook.c's tests: no hook module (nn/module.h), as the runtime's list (src/nx/patches/modules.c) has none, and
   no library's (module_lib.h) */
#include "nx/nn/module.h"
#include "nx/nn/module_lib.h"
#include "nx/loader/hook_routes.h"

const HookModule* const g_hook_modules[] = {NULL};
void hook_module_libs_load(int (*off)(const char* name)) { (void)off; }
u32 hook_lib_cpu_count(void) { return 0; }
HookLibCpu* hook_lib_cpu(u32 i) { return (void)i, NULL; }
void hook_lib_cpu_run(u32 i, CPU* c, u64 entry) { (void)i, (void)c, (void)entry; }
int snx_hook_build_key(const unsigned char build_id[32], SnxU64 offset, char* out, size_t size) {
    return (void)build_id, (void)offset, (void)out, (void)size, 0;
}
