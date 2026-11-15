/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* hook_install.c's module cases: one hook module, named after tests/hook/install.hook's plain family, which applies
   when main's code at +0x40 holds the four bytes it checks */
#include "nx/hle/hook/module.h"

static const HookModuleCode code[] = {{NULL, 0x40, "de ad be ef"}};
static const HookModule plain = {"plain", "the test spec's plain family as a module", 0, NULL, NULL, code, 1, NULL, 0};
const HookModule* const g_hook_modules[] = {&plain, NULL};
