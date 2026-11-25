/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The part of the PE/COFF format that link_coff.c (the DLL linker) and code_loader_coff.c (the JIT loader's COFF
   objects) read x86-64 and ARM64 objects with, read byte by byte so both build anywhere */
#ifndef SNX_RECOMPILER_OBJECT_COFF_DEFS_H
#define SNX_RECOMPILER_OBJECT_COFF_DEFS_H
#include "common/utilities/endian.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SCN_CODE          0x00000020u
#define SCN_INITIALIZED   0x00000040u
#define SCN_UNINITIALIZED 0x00000080u
#define SCN_LNK_INFO      0x00000200u
#define SCN_LNK_REMOVE    0x00000800u
#define SCN_LNK_COMDAT    0x00001000u
#define SCN_ALIGN_MASK    0x00F00000u
#define SCN_NRELOC_OVFL   0x01000000u
#define SCN_DISCARDABLE   0x02000000u
#define SCN_EXECUTE       0x20000000u
#define SCN_READ          0x40000000u
#define SCN_WRITE         0x80000000u
#define SYM_EXTERNAL      2
#define SYM_STATIC        3
#define SYM_WEAK_EXTERNAL 105
#define SEL_ASSOCIATIVE   5
#define REL_ABSOLUTE      0
#define REL_ADDR64        1
#define REL_ADDR32        2
#define REL_ADDR32NB      3
#define REL_REL32         4
#define REL_REL32_5       9
#define REL_SECTION       10
#define REL_SECREL        11

/* the objects' machines: IMAGE_FILE_MACHINE_AMD64 and IMAGE_FILE_MACHINE_ARM64 */
#define COFF_X64               0x8664u
#define COFF_ARM64             0xAA64u
/* ARM64's relocation types (IMAGE_REL_ARM64_*); the REL_ ones above are x86-64's */
#define REL_A64_ABSOLUTE       0x00
#define REL_A64_ADDR32         0x01
#define REL_A64_ADDR32NB       0x02
#define REL_A64_BRANCH26       0x03
#define REL_A64_PAGEBASE_REL21 0x04
#define REL_A64_REL21          0x05
#define REL_A64_PAGEOFFSET_12A 0x06
#define REL_A64_PAGEOFFSET_12L 0x07
#define REL_A64_SECREL         0x08
#define REL_A64_SECREL_LOW12A  0x09
#define REL_A64_SECREL_HIGH12A 0x0A
#define REL_A64_SECREL_LOW12L  0x0B
#define REL_A64_SECTION        0x0D
#define REL_A64_ADDR64         0x0E
#define REL_A64_BRANCH19       0x0F
#define REL_A64_BRANCH14       0x10
#define REL_A64_REL32          0x11

/* Writes ARM64 relocation type at p, room bytes before its section's end, the place at address P: S the symbol's
   address, base what ADDR32NB counts from (the image's base, 0 when S is already an RVA) and section_start the start
   of S's section, which the SECREL types count from. The addend is in the field, as COFF keeps it: ADRP's and ADR's
   immediate in bytes, an ADD's 12 bits, a load or store's 12 bits scaled by its access size, a branch's offset in
   instructions, the data word itself. ADDR64, ADDR32 and SECTION are the caller's to write (an image's base
   relocations, its section numbers). 0, or SNX_LINK_OUTSIDE when the field is past the section, SNX_LINK_RANGE when
   the value does not fit it (a B or BL beyond 128 MiB goes through a veneer: snx_coff_a64_veneer),
   SNX_LINK_MISALIGNED for a load or store offset its size does not divide or a branch to an unaligned target,
   SNX_LINK_UNSUPPORTED for another type; nothing is written then (link_coff.c) */
int snx_coff_a64_relocate(unsigned char* p, size_t room, unsigned type, uint64_t S, uint64_t P, uint64_t base,
                          uint64_t section_start);
/* A veneer, what a B or BL reaches a target beyond 128 MiB through: ADRP x16, target; then ADD x16, x16,
   :lo12:target, or when indirect LDR x16, [x16, :lo12:target] (the address held in the 8-byte slot at target); then
   BR x16. x16 is IP0, which the calling convention lets a veneer clobber */
#define SNX_COFF_A64_VENEER_SIZE 12
/* Writes at v the veneer placed at address at: 0, or SNX_LINK_RANGE beyond ADRP's 4 GiB, SNX_LINK_MISALIGNED for an
   indirect slot not 8-aligned (link_coff.c) */
int snx_coff_a64_veneer(unsigned char* v, uint64_t at, uint64_t target, int indirect);

/* an 18-byte symbol record's name into out: its 8 bytes inline, or (first word 0) the string table's at the offset in
   the second word, empty when it is outside a table of strings_size bytes */
static inline void snx_coff_symbol_name(const char* strings, uint32_t strings_size, const unsigned char* s, char* out,
                                        size_t size) {
    size_t n = 0;
    if (!snx_ld_le32(s)) {
        uint32_t offset = snx_ld_le32(s + 4);
        snprintf(out, size, "%s", offset < strings_size ? strings + offset : "");
        return;
    }
    while (n < 8 && s[n]) ++n;
    if (n >= size) n = size - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

/* qsort's order of .pdata entries (RUNTIME_FUNCTION: begin, end and unwind data on x86-64, begin and unwind data on
   ARM64) by their begin address, the bytes read as the little-endian word they are: RtlLookupFunctionEntry
   binary-searches a table in that order */
static inline int snx_coff_compare_function(const void* a, const void* b) {
    uint32_t x = snx_ld_le32(a), y = snx_ld_le32(b);
    return x < y ? -1 : x > y;
}

/* the size of a .pdata entry (RUNTIME_FUNCTION) of machine's objects */
static inline size_t snx_coff_function_size(unsigned machine) { return machine == COFF_ARM64 ? 8 : 12; }

#endif
