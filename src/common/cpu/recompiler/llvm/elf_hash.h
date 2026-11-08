/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The two hashes an ELF symbol table is looked up by: the System V ABI's DT_HASH one (generic ABI 5, "Hash Table") and
   GNU's DT_GNU_HASH one. Apart from elf_defs.h, so a file that has the system's <elf.h> can include it too */
#ifndef SNX_RECOMPILER_LLVM_ELF_HASH_H
#define SNX_RECOMPILER_LLVM_ELF_HASH_H
#include <stdint.h>

/* DT_HASH's: each byte shifted in, the top nibble folded back and cleared, so the value stays below 2^28 */
static inline uint32_t snx_elf_sysv_hash(const char* name) {
    uint32_t h = 0;
    for (const unsigned char* p = (const unsigned char*)name; *p; ++p) {
        h = (h << 4) + *p;
        uint32_t high = h & 0xf0000000u;
        h ^= high >> 24;
        h &= ~high;
    }
    return h;
}
/* DT_GNU_HASH's: h * 33 + c from 5381 */
static inline uint32_t snx_elf_gnu_hash(const char* name) {
    uint32_t h = 5381;
    for (const unsigned char* p = (const unsigned char*)name; *p; ++p) h = h * 33 + *p;
    return h;
}
#endif
