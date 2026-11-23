/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Hook modules from libraries (docs/platforms/nx/hooks.md, "Modules in libraries"): each library of the hook modules'
   folder is a module that snx_hook_module.h describes. Its file, entry point and ABI are checked as
   nx/loader/hook_libs.h does it, and module.c's list gets it after the compiled-in ones. Its CPU hooks are kept here,
   and hook_install puts each one of a module that applies in its function's dispatch slot, with one adapter for all
   of them (hook.c) that runs the module's handler with the guest CPU. A library that is refused is closed, with the
   reason in the log. One that is taken stays loaded for the run */
#ifndef SNX_HLE_HOOK_MODULE_LIB_H
#define SNX_HLE_HOOK_MODULE_LIB_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/hle/hook/module.h"
#include "nx/hle/hook/snx_hook_module.h"
#include "nx/loader/hook_libs.h"

/* The libraries of the folder the hook_modules_dir setting names, or of hook_modules/ in the user's data folder when
   it names none, in their names' order: once in a run, at start (hook_install), before hook_modules_select. off(name)
   says that a module is off (hooks_off): its library is never loaded, so none of its code runs */
void hook_module_libs_load(int (*off)(const char* name));
/* the libraries in folder (files that end as the system's libraries do): how many modules were added */
u32 hook_module_libs_load_dir(const char* folder, int (*off)(const char* name));
/* the library at path: 1 when its module was added, 0 when it was refused or is off (the log says which) */
int hook_module_lib_load(const char* path, int (*off)(const char* name));

/* a library's CPU hook as the runtime took it: its module's index in the list (hook_module_at), which
   hook_module_applies says applies, and that module's name, the game's module it is in (NULL for main), its offset
   there, its key (BUILDID:OFFSET of that game module's whole ID, as the route list names it, which hook_install
   writes) and the library's hook */
typedef struct {
    u32 module;
    const char* owner;
    const char* in;
    u64 offset;
    char key[SNX_HOOK_KEY];
    const SnxHookCpuHook* hook;
} HookLibCpu;
u32 hook_lib_cpu_count(void);
HookLibCpu* hook_lib_cpu(u32 i);
/* CPU hook i's handler for the call at entry, with the registers as the call left them (hook.c's adapter, which then
   returns to the caller's x30) */
void hook_lib_cpu_run(u32 i, CPU* c, u64 entry);

/* the guest CPU a handler gets: valid while the handler runs (active) */
struct SnxHookCpu {
    CPU* c;
    u64 entry;
    int active, ran;
    u32 hook; /* the CPU hook's index (hook_lib_cpu) */
};
/* the host a library's entry point and its handlers get (snx_hook_module.h), the same for every library and the run */
const SnxHookHost* hook_module_lib_host(void);
#endif
