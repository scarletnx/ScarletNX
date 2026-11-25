/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The hook module libraries (hle/hook/snx_hook_module.h, docs/platforms/nx/hooks.md "Modules in libraries") as the
   runtime and the recompile read them: which files of the hook modules folder are libraries, the module name a file
   gives, a library opened with its entry point called and its ABI version, name and CPU hooks checked. The runtime
   then makes each one a module of its own (hle/hook/module_lib.c). The recompile adds the keys of their CPU hooks to
   the route list (hook_routes.h), so the functions they hook are called through their slots */
#ifndef SNX_NX_HOOK_LIBS_H
#define SNX_NX_HOOK_LIBS_H
#include <stddef.h>
#include "nx/hle/hook/snx_hook_module.h"

#define SNX_HOOK_LIBS_FOLDER "hook_modules"
enum { SNX_HOOK_LIB_NAME = 63, SNX_HOOK_KEY = 96 };

/* the hook modules folder: setting when it names one (the hook_modules_dir setting), else hook_modules/ in the user's
   data folder. NULL when there is none. The caller frees it */
char* snx_hook_libs_folder(const char* setting);
/* whether file (a file name) ends as the system's libraries do: .so, .dll on Windows, .dylib on macOS */
int snx_hook_lib_is_library(const char* file);
/* the file name of path, past its last folder separator */
const char* snx_hook_lib_file(const char* path);
/* the module name of a library's file name, its name without the suffix: 1, or 0 with the reason in why when it is
   longer than SNX_HOOK_LIB_NAME or has a character other than a to z, 0 to 9 and _ */
int snx_hook_lib_name(const char* file, char name[SNX_HOOK_LIB_NAME + 1], char* why, size_t size);
/* The library at path, whose module must be named name, opened, its snx_hook_module called with host, and what it
   returned checked: the ABI's major and minor, the name, its code checks (at most SNX_HOOK_CODES, each with bytes) and
   its CPU hooks (1 to SNX_HOOK_CPU_HOOKS, in the build its build ID names, each with a handler and a name, at a
   4-aligned offset). NULL with *library and *module, or the reason, the library then closed */
const char* snx_hook_lib_open(const char* path, const char* name, const SnxHookHost* host, void** library,
                              const SnxHookModule** module, char* why, size_t size);
/* how many CPU hooks m has */
unsigned snx_hook_lib_cpu_count(const SnxHookModule* m);
/* The recompile's: every library of folder opened with a host that has the log alone (no handler runs), its module's
   CPU hooks added to the route list by their build ID and offset (snx_hook_route_add_build), and the library closed.
   say gets a line for each library (no newline). How many CPU hooks the libraries named */
size_t snx_hook_libs_route(const char* folder, void (*say)(const char* line));
#endif
