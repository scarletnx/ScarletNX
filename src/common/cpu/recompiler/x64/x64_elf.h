/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A relocatable object built in memory, ELF or Windows' COFF, for what the native back ends write: an AOT
   module's descriptor (nx/loader/module_x64.c), AArch32 blocks (mach/mach_arm.inc) and, as COFF, the guest's functions
   (x64_codegen.c; mach/mach_codegen.inc writes their ELF objects itself). Four sections of bytes (.text, .rodata,
   .data.rel.ro, .data) and .bss of a size, symbols at their offsets, relocations by symbol name (a name no symbol
   defines is left undefined, for the link) */
#ifndef SNX_RECOMPILER_X64_ELF_H
#define SNX_RECOMPILER_X64_ELF_H
#include <stddef.h>

typedef struct SnxXElf SnxXElf;
enum { SNX_ELF_TEXT, SNX_ELF_RODATA, SNX_ELF_RELRO, SNX_ELF_DATA, SNX_ELF_BSS, SNX_ELF_SECTIONS };
enum { SNX_ELF_LOCAL = 0, SNX_ELF_GLOBAL = 1, SNX_ELF_HIDDEN = 2, SNX_ELF_FUNC = 4 };

SnxXElf* snx_xelf_new(void);
void snx_xelf_free(SnxXElf* o);
/* the object's ELF machine (EM_X86_64 by default, EM_AARCH64...), whose relocation numbers snx_xelf_reloc takes */
void snx_xelf_set_machine(SnxXElf* o, unsigned machine);
/* the object's e_flags (0 by default): the host's ABI (RISC-V's float ABI, PowerPC's ELFv2) */
void snx_xelf_set_flags(SnxXElf* o, unsigned flags);
/* the object a big-endian machine's (1): its tables in that order wherever it's written */
void snx_xelf_set_big(SnxXElf* o, int big);
/* the object COFF, for Windows (1; a Windows host's default), x86-64's or (its machine EM_AARCH64) arm64's: its
   relocations' types are the machine's ELF ones all the same (R_X86_64_64, PC32 and PLT32; R_AARCH64_ABS64,
   ADR_PREL_PG_HI21, ADD_ABS_LO12_NC, LDST8_ABS_LO12_NC, LDST64_ABS_LO12_NC, CALL26 and JUMP26), its two read-only
   sections .rdata. A call of .text's to a function of its own is written, with no relocation. An ADRP's addend is under
   1 MiB where its symbol is another object's */
void snx_xelf_set_coff(SnxXElf* o, int coff);
/* a COFF object's function table (.pdata, .xdata): the code of .text from begin to end unwinds as the prologue at code
   (begin's bytes) says. x86-64's is x64_codegen.c's or x64_arm.c's: the pushes, the frame and the XMM registers saved;
   the code has its jumps in there, but an epilogue's: out of it Windows' unwinder takes one for that. arm64's is
   arm64_codegen.c's or arm64_arm.c's: the pairs saved and the frame, in size bytes to end that hold every epilogue of
   theirs, each followed at once by its return or its jump */
void snx_xelf_unwind(SnxXElf* o, size_t begin, size_t end, const unsigned char* code, size_t size);
/* n bytes (none: zeros; .bss takes none) at an offset of align, which it gives */
size_t snx_xelf_put(SnxXElf* o, int section, const void* bytes, size_t n, size_t align);
/* the section's bytes so far, to patch (not .bss) */
unsigned char* snx_xelf_bytes(SnxXElf* o, int section);
/* a symbol of flags (SNX_ELF_GLOBAL, HIDDEN, FUNC) at offset in section, of size bytes */
void snx_xelf_symbol(SnxXElf* o, const char* name, int section, size_t offset, size_t size, int flags);
/* at offset in section, the R_X86_64_* type's relocation of the symbol name plus addend */
void snx_xelf_reloc(SnxXElf* o, int section, size_t offset, const char* name, unsigned type, long long addend);
/* the object (malloc'd), 0; -1 out of memory, -2 for a COFF object whose relocation or prologue COFF has no form of */
int snx_xelf_write(SnxXElf* o, unsigned char** out, size_t* size);

#endif
