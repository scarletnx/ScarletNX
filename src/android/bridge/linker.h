/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_LINKER_H
#define SNX_ANDROID_LINKER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The guest's dynamic linker, as bionic's loads a library (docs/android/NATIVE.md, "Loading"): ELF64 AArch64 shared
   objects mapped into the code window, their DT_NEEDED first (the app's own from the library folder, the system's as
   HLE libraries), relocated, and their initializers run. Symbols resolve in the global group, then each library's own
   tree. Every function is safe from any thread: one library loads at a time */

typedef struct GuestLibrary GuestLibrary;

/* the folder of the app's own libraries, which DT_NEEDED and dlopen of a bare name look in */
void linker_set_library_dir(const char* dir);
/* each library and its load address on stderr (--linker-verbose) */
void linker_set_verbose(bool verbose);

/* the library at path (or a bare name: the app's folder, then the HLE libraries) loaded, or found loaded. NULL with
   dlerror's text in error */
GuestLibrary* linker_load(const char* path, char* error, size_t error_size);
/* a symbol's guest address in a library and its DT_NEEDED tree (RTLD_DEFAULT when library is NULL); 0 for none */
uint64_t linker_symbol(GuestLibrary* library, const char* name);
/* the library's soname or file name */
const char* linker_name(const GuestLibrary* library);
/* each library loaded, in load order, until fn returns nonzero (its result then, else 0): with its base, its name, and
   its program headers in guest memory (count of them) */
int linker_each(int (*fn)(void* context, uint64_t base, const char* name, uint64_t phdr, unsigned count),
                void* context);
/* the library that holds a guest address, NULL for none */
GuestLibrary* linker_owner(uint64_t address);
/* a symbol of the libraries loaded after the one that holds caller (dlsym's RTLD_NEXT) */
uint64_t linker_next_symbol(uint64_t caller, const char* name);

/* the library and the symbol nearest below a guest address, for dladdr and diagnostics: false outside them */
bool linker_address(uint64_t address, const char** library, uint64_t* base, const char** symbol, uint64_t* symbol_at);
#endif
