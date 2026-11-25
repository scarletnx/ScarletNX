/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Hook modules (docs/platforms/nx/hooks.md, "Modules"): a game's patch as one unit that says what it applies to (a
   title, a module's build, code it checks) and what it hooks on the CPU: the address hooks of its family, in
   src/nx/patches/<name>.hook, registered in one list (src/nx/patches/modules.c), or a library's CPU hooks
   (module_lib.h), whose modules come after them. At start each is applied, or refused with the reason in the log, and
   a refused one leaves the game as it is: its hooks aren't installed. hooks=0 turns every module off, and hooks_off
   names the modules turned off, as it names families */
#ifndef SNX_HLE_HOOK_MODULE_H
#define SNX_HLE_HOOK_MODULE_H
#include "nx/loader/module_abi.h"

/* code a module checks before it applies: bytes at an offset from the base of one of the game's modules, a byte as two
   hex digits or ?? for any, spaces between them free ("fd 7b bf a9 ?? ?? 00 94"). All must hold */
typedef struct {
    const char* module; /* the game's module by its name (SnxModInfo.name), NULL for the hook module's own */
    u64 offset;
    const char* bytes;
} HookModuleCode;

typedef struct {
    const char* name;     /* its word in hooks_off and in the log, the family of its address hooks */
    const char* what;     /* what it does, for the log */
    u64 title;            /* the title ID it applies to, 0 any */
    const char* module;   /* the game's module its build ID and checks name: NULL for main */
    const char* build_id; /* that module's build ID, the hex of its first bytes (as an address hook's key), NULL any */
    const HookModuleCode* code;
    u32 ncode;
} HookModule;

/* the list: src/nx/patches/modules.c's, ending in NULL. A test links its own */
extern const HookModule* const g_hook_modules[];
/* module i of every module: the compiled-in list's, then those hook_module_add added (the libraries' modules,
   module_lib.h), NULL past the last */
const HookModule* hook_module_at(u32 i);
/* m after the others, for the run: its index (hook_module_at), or -1 when the list holds as many modules as it can */
int hook_module_add(const HookModule* m);

/* Which of the list apply to the game whose modules mod lists, in guest memory at mem: each logged, applied or not
   and why. off(name) says whether a module is turned off (hook.c's hook_family_off). Run once at start
   (hook_install), before the game's threads */
void hook_modules_select(const SnxModule* mod, const u8* mem, int (*off)(const char* name));
/* whether module i (hook_module_at) applies, after hook_modules_select, and whether a family is a compiled-in
   module's that doesn't (a library's module has no CPU hooks, so no family is its own) */
int hook_module_applies(u32 i);
int hook_module_family_refused(const char* family);
/* why m doesn't apply to the game mod lists, or NULL: for hook_modules_select and the tests */
const char* hook_module_refusal(const HookModule* m, const SnxModule* mod, const u8* mem);
#endif
