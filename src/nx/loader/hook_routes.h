/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The route list: the symbols whose calls the recompiler sends through their dispatch slots, where a hook of the
   runtime's can take them. They are the symbols that hle/hook/hooks.hook and gpu/nvn/nvn.hook name, in each version,
   with their route lines (hookgen --routes), and the keys of the address hooks of src/nx/patches/ (BUILDID:OFFSET),
   each of which routes one function of one build of a module. Every other call to a recompiled function is a direct
   call, which no hook sees. A module carries the list it was built with (snx_routes, module_abi.h), and hook_install
   says which hook of the runtime's the module calls directly */
#ifndef SNX_NX_HOOK_ROUTES_H
#define SNX_NX_HOOK_ROUTES_H
#include "common/utilities/common.h"
#include <stddef.h>

/* whether the list names the symbol, or holds a pattern (hookgen's '*') that matches it */
int snx_hook_routed(const char* name);
/* whether name matches pattern, whose each '*' matches any run of characters (none too) */
static inline int snx_hook_glob(const char* pattern, const char* name) {
    const char *star = NULL, *resume = NULL;
    while (*name) {
        if (*pattern == '*')
            star = ++pattern, resume = name; /* the run starts empty */
        else if (*pattern == *name)
            pattern++, name++;
        else if (star)
            pattern = star, name = ++resume; /* the last '*' takes one more character */
        else
            return 0;
    }
    while (*pattern == '*') pattern++;
    return !*pattern;
}

/* the symbols, sorted by strcmp, then the keys snx_hook_route_add added in this process */
size_t snx_hook_route_count(void);
const char* snx_hook_route_name(size_t index);
/* An address hook's key added to the list for this process (route_addresses adds a library CPU hook's, below). A key
   the list holds already is not added again */
void snx_hook_route_add(const char* key);
/* A hook module library's CPU hook (hook_libs.h): build_id is the hex of a build ID's first bytes as the module gives
   it (the whole ID, or a prefix of it), offset the function's. The recompile adds them before it reads the game's
   modules. The loader then routes each one in every module whose ID begins with those bytes, under the key of that
   module's whole ID (snx_hook_build_key), which it adds to the list, so the module's snx_routes carries the key the
   runtime looks the hook up by, however much of the ID the library gave. A pair added already is not added again */
void snx_hook_route_add_build(const char* build_id, SnxU64 offset);
size_t snx_hook_route_build_count(void);
/* entry i's build ID text as the library gave it, and its offset in *offset: NULL past the last */
const char* snx_hook_route_build_entry(size_t i, SnxU64* offset);
/* whether the list holds key (a symbol of the generated list, or a key added) */
int snx_hook_route_listed(const char* key);
/* entry i's offset, when build_id begins with its bytes: 1, else 0 */
int snx_hook_route_build_at(size_t i, const unsigned char build_id[32], SnxU64* offset);
/* whether hex is a build ID's first bytes in hex: 2 to 64 digits, an even count */
int snx_hook_build_hex(const char* hex);
/* The key of the function at offset in the module built as build_id, as hookgen writes an address hook's, into out
   (size bytes): the ID's hex in lower case up to its last byte that isn't zero (one byte at least), ':', the offset in
   hex without leading zeros. 1, or 0 for an out too small */
int snx_hook_build_key(const unsigned char build_id[32], SnxU64 offset, char* out, size_t size);
/* a symbol's hash: 64-bit FNV-1a of its bytes */
SnxU64 snx_hook_route_hash(const char* name);
/* the list's hash: FNV-1a over the hash of each symbol, in order */
SnxU64 snx_hook_route_list_hash(void);
/* 1 when name is an address hook's key (hookgen's BUILDID:OFFSET: the build ID's first bytes in hex, the function's
   offset in its module in hex) for the module built as build_id, which holds those bytes and zeros after them, with
   *offset the function's; 0 for a symbol, a key of another build or a malformed one */
int snx_hook_key_at(const char* name, const unsigned char build_id[32], SnxU64* offset);
#endif
