/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What only LLVM left in a user's folders, now that nothing makes or reads it: each file recognized by its format and
   its key, never by its name alone. Nothing here deletes a file by itself: the launcher asks first (its old files'
   page and a game's Delete Old Caches), and a runtime only says what it found and where it is removed */
#ifndef SNX_UTILITIES_OLD_LLVM_CACHES_H
#define SNX_UTILITIES_OLD_LLVM_CACHES_H
#include <stddef.h>
#include <stdint.h>

/* where the launcher deletes them, as a runtime says it: units/'s and the library caches' on Settings > App's old
   files, a game's .jitcache in its box among the Games */
#define SNX_LLVM_CACHES_WHERE    "the launcher deletes them in Settings > App > Move Old Files"
#define SNX_LLVM_CACHE_JIT_WHERE "the launcher deletes it in the game's box in Games, Delete Old Caches"

typedef enum {
    SNX_LLVM_CACHE_UNITS,  /* units/ of the cache folder: <64 hex>.o, a relocatable object, an older version's unit */
    SNX_LLVM_CACHE_RTLIB,  /* a library cache's rtlib-<32 hex>.o, a relocatable object: LLVM's runtime library */
    SNX_LLVM_CACHE_MODULE, /* a library cache's <32 hex>.so, .dll or .objs whose descriptor is LLVM's snx_lib_module */
    SNX_LLVM_CACHE_JIT, /* a JIT cache (SNXJIT01) whose key names LLVM (llvm=): a game's .jitcache, library-jit.cache */
    SNX_LLVM_CACHE_KINDS
} SnxLlvmCacheKind;
typedef struct {
    char* path;
    uint64_t bytes;
    SnxLlvmCacheKind kind;
} SnxLlvmCache;
typedef struct {
    SnxLlvmCache* files;
    size_t count;
    uint64_t bytes;
} SnxLlvmCaches;

/* 1 when the file at path is kind's: its name of that form and its contents of that format and key */
int snx_llvm_cache_is(const char* path, SnxLlvmCacheKind kind);
/* The files of the cache folder cache_root (the user's cache, SNX_USER_CACHE) only LLVM left: units/'s, and those of
   the library caches the Linux runtime and the Android driver keep there (linux-jit/, android-jit/). Appended to
   found; 0, or -1 out of memory */
int snx_llvm_caches_scan(const char* cache_root, SnxLlvmCaches* found);
/* The files of one library cache folder (the Linux runtime's linux-jit/, the Android driver's android-jit/, or the
   folder --jit-cache names) only LLVM left: its runtime library, its modules and its library-jit.cache. Appended to
   found; 0, or -1 out of memory */
int snx_llvm_caches_library(const char* folder, SnxLlvmCaches* found);
/* A JIT cache file (a game's .jitcache) appended to found when it is LLVM's; 1 when it was, 0 when not, -1 out of
   memory */
int snx_llvm_caches_jit(const char* path, SnxLlvmCaches* found);
/* the files of found in folder alone (the found that lists that folder's), how many and their bytes */
size_t snx_llvm_caches_in(const SnxLlvmCaches* found, const char* folder, uint64_t* bytes);
/* Each file of found deleted, when it is still of its kind (snx_llvm_cache_is): how many went, their bytes in *bytes;
   the first that could not, its path and the system's reason, in error ("" when every one went) */
size_t snx_llvm_caches_delete(const SnxLlvmCaches* found, uint64_t* bytes, char* error, size_t error_size);
/* What a runtime says of found's files in folder (every one of found with folder NULL, named by the first's path), which
   it never deletes, and where the launcher deletes them: "<n> file(s), <x.x> MB of LLVM's old caches in <folder>, which
   nothing reads now: <where>" into out. How many; with none, out is "" */
size_t snx_llvm_caches_said(const SnxLlvmCaches* found, const char* folder, const char* where, char* out, size_t size);
void snx_llvm_caches_free(SnxLlvmCaches* found);

#endif
