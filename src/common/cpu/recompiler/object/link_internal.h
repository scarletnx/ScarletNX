/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the linkers (link_elf.c, link_macho.c, link_coff.c), the ELF object reader (elf_object.c) and the JIT loader
   (code_loader.c, code_loader_coff.c) share beside link.h: their error, the keys of their GOT slots and veneers, and
   the AArch64 instruction fields their relocations write */
#ifndef SNX_RECOMPILER_OBJECT_LINK_INTERNAL_H
#define SNX_RECOMPILER_OBJECT_LINK_INTERNAL_H
#include "common/cpu/recompiler/object/link.h"
#include "common/utilities/endian.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* the message in error, -1 */
static inline int snx_link_fail(char* error, size_t error_size, const char* format, const char* a, const char* b) {
    snprintf(error, error_size, format, a, b);
    return -1;
}

/* a GOT slot's key: its definition's "object:symbol" */
static inline void snx_link_got_key(char* key, size_t object, size_t index) { sprintf(key, "%zu:%zu", object, index); }
/* a veneer's key: its caller's object and its target's definition */
static inline void snx_link_veneer_key(char* key, size_t caller, size_t object, size_t index) {
    sprintf(key, "%zu:%zu:%zu", caller, object, index);
}

/* ---- AArch64 instruction fields. Everything in uint64_t: a negative displacement is its two's complement, which
   the masks cut to the field as the instruction wants it, with no signed shift or overflow on the way ---- */

/* the little-endian instruction at p with value in its bits from shift, width wide */
static inline void snx_link_field(unsigned char* p, uint64_t value, unsigned shift, unsigned width) {
    uint32_t mask = (uint32_t)((((uint64_t)1 << width) - 1) << shift);
    snx_st_le32(p, (snx_ld_le32(p) & ~mask) | ((uint32_t)(value << shift) & mask));
}
/* ADR and ADRP's 21-bit immediate: its low two bits at 29, the rest at 5 */
static inline void snx_link_adr_field(unsigned char* p, uint64_t value) {
    snx_link_field(p, value & 3, 29, 2);
    snx_link_field(p, value >> 2, 5, 19);
}
static inline uint64_t snx_link_page_of(uint64_t address) { return address & ~(uint64_t)0xFFF; }

#endif
