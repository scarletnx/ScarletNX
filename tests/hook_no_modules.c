/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* hook.c's tests: no hook module (hle/hook/module.h), as the runtime's list (src/nx/patches/modules.c) has none */
#include "nx/hle/hook/module.h"

const HookModule* const g_hook_modules[] = {NULL};
