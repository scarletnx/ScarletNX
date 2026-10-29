/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The route list: the symbols whose calls the recompiler sends through their dispatch slots, where a hook of the
   runtime's can take them. They are the symbols that hle/hook/hooks.hook and gpu/nvn/nvn.hook name, in each version,
   with their route lines (hookgen --routes). Every other call to a recompiled function is a direct call, which no hook
   sees. A module carries the list it was built with (snx_routes, module_abi.h), and hook_install says which hook of
   the runtime's the module calls directly */
#ifndef SNX_NX_HOOK_ROUTES_H
#define SNX_NX_HOOK_ROUTES_H
#include "common/utilities/common.h"
#include <stddef.h>

/* whether the list names the symbol */
int snx_hook_routed(const char* name);
/* the symbols, sorted by strcmp */
size_t snx_hook_route_count(void);
const char* snx_hook_route_name(size_t index);
/* a symbol's hash: 64-bit FNV-1a of its bytes */
SnxU64 snx_hook_route_hash(const char* name);
/* the list's hash: FNV-1a over the hash of each symbol, in order */
SnxU64 snx_hook_route_list_hash(void);
#endif
