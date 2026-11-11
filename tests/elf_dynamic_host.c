/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* elf_dynamic.c's test (elf_dynamic.c here) on this process's own dynamic linker, found by the address the kernel gave
   (AT_BASE) as a run finds Android's: a symbol it defines is where dlsym says, and one it has not is none. On glibc
   for x86_64 and arm64, whose dynamic linker has the GNU hash table; a file of its own, as the system's <elf.h> comes
   with <sys/auxv.h> and the test has elf_defs.h's names */
#include "common/utilities/elf_dynamic.h"
#include <assert.h>
#include <stdio.h>
#if defined(__GLIBC__) && (defined(__x86_64__) || defined(__aarch64__))
#include <dlfcn.h>
#include <sys/auxv.h>
#define TEST_HOST_LINKER 1
#endif

const char* elf_dynamic_host(void);
const char* elf_dynamic_host(void) {
#ifdef TEST_HOST_LINKER
    static char found[160];
    const void* base = (const void*)getauxval(AT_BASE);
    void* debug = dlsym(RTLD_DEFAULT, "_r_debug");
    assert(base && debug && snx_elf_loaded_symbol(base, "_r_debug") == debug);
    assert(!snx_elf_loaded_symbol(base, "__loader_android_create_namespace"));
    snprintf(found, sizeof found, "the dynamic linker at %p has _r_debug at %p", (void*)base, debug);
    return found;
#else
    return "this system's dynamic linker is not looked up (glibc on x86_64 or arm64 only)";
#endif
}
