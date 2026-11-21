/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A relocatable ELF x86-64 object built in memory, for what the x86-64 back end writes without LLVM besides the
   guest's functions (x64_codegen.c writes those itself): an AOT module's descriptor (nx/loader/module_x64.c). Four
   sections of bytes (.text, .rodata, .data.rel.ro, .data) and .bss of a size, symbols at their offsets, relocations
   by symbol name (a name no symbol defines is left undefined, for the link) */
#ifndef SNX_RECOMPILER_X64_ELF_H
#define SNX_RECOMPILER_X64_ELF_H
#include <stddef.h>

typedef struct SnxXElf SnxXElf;
enum { SNX_ELF_TEXT, SNX_ELF_RODATA, SNX_ELF_RELRO, SNX_ELF_DATA, SNX_ELF_BSS, SNX_ELF_SECTIONS };
enum { SNX_ELF_LOCAL = 0, SNX_ELF_GLOBAL = 1, SNX_ELF_HIDDEN = 2, SNX_ELF_FUNC = 4 };

SnxXElf* snx_xelf_new(void);
void snx_xelf_free(SnxXElf* o);
/* n bytes (none: zeros; .bss takes none) at an offset of align, which it gives */
size_t snx_xelf_put(SnxXElf* o, int section, const void* bytes, size_t n, size_t align);
/* the section's bytes so far, to patch (not .bss) */
unsigned char* snx_xelf_bytes(SnxXElf* o, int section);
/* a symbol of flags (SNX_ELF_GLOBAL, HIDDEN, FUNC) at offset in section, of size bytes */
void snx_xelf_symbol(SnxXElf* o, const char* name, int section, size_t offset, size_t size, int flags);
/* at offset in section, the R_X86_64_* type's relocation of the symbol name plus addend */
void snx_xelf_reloc(SnxXElf* o, int section, size_t offset, const char* name, unsigned type, long long addend);
/* the object (malloc'd), 0; -1 out of memory */
int snx_xelf_write(SnxXElf* o, unsigned char** out, size_t* size);

#endif
