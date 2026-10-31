/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The LLVM backend's linker: relocatable objects (as LLVM and Clang write them: x86-64, AArch64 or PowerPC64 ELF on
   Unix and Android, x86-64 COFF on Windows, arm64 Mach-O on macOS and iOS) into the game module the runtime loads, an
   ELF shared object, a PE DLL or a Mach-O dylib. The module defines everything it uses (rtlib.c), so the linker
   resolves every symbol itself and writes no imports: only the relocations position-independent code needs (ELF
   R_*_RELATIVE, PE base relocations, Mach-O rebases) and an export table */
#ifndef SNX_RECOMPILER_LLVM_LINK_H
#define SNX_RECOMPILER_LLVM_LINK_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const unsigned char* data;
    size_t size;
    const char* name; /* for diagnostics */
} SnxLinkInput;

typedef struct {
    unsigned char* data; /* malloc'd */
    size_t size;
} SnxLinkOutput;

/* Links by the format of the first input. exports name defined functions the shared library exports (by their C
   names: Mach-O's carry an underscore). soname is the library's own file name (the PE export directory records it,
   a dylib's install name and code signature are it). 0, or -1 with the reason in error */
int snx_link(const SnxLinkInput* inputs, size_t count, const char* const* exports, size_t export_count,
             const char* soname, SnxLinkOutput* output, char* error, size_t error_size);
/* An ELF code section named this prefix and a decimal rank (".text.snx_hot.0", ...) goes before all the other code,
   in the order of the ranks: a profile's hot functions, each in its own section, packed together (the recompiler's
   --layout). The ELF linker only, and only for a module without veneers: an AArch64 or PowerPC64 module large enough
   to need them keeps each section with its object, in reach of the object's island */
#define SNX_LINK_HOT_TEXT ".text.snx_hot."
int snx_link_elf(const SnxLinkInput* inputs, size_t count, const char* const* exports, size_t export_count,
                 const char* soname, SnxLinkOutput* output, char* error, size_t error_size);
int snx_link_coff(const SnxLinkInput* inputs, size_t count, const char* const* exports, size_t export_count,
                  const char* soname, SnxLinkOutput* output, char* error, size_t error_size);
int snx_link_macho(const SnxLinkInput* inputs, size_t count, const char* const* exports, size_t export_count,
                   const char* soname, SnxLinkOutput* output, char* error, size_t error_size);

/* shared by the linkers: a growable byte buffer and a name -> value map */
typedef struct {
    unsigned char* data;
    size_t size, capacity;
} SnxLinkBuffer;
void snx_link_reserve(SnxLinkBuffer* buffer, size_t size); /* grows to size bytes, zero-filled */
size_t snx_link_append(SnxLinkBuffer* buffer, const void* data, size_t size);
void snx_link_align(SnxLinkBuffer* buffer, size_t alignment);

typedef struct {
    char** keys;
    size_t* values;
    size_t count, capacity;
} SnxLinkNames;
/* 1 and *value when present */
int snx_link_find(const SnxLinkNames* names, const char* key, size_t* value);
void snx_link_insert(SnxLinkNames* names, const char* key, size_t value);
void snx_link_names_free(SnxLinkNames* names);

/* The writes relocations come down to, shared by link_elf.c, link_coff.c and the JIT loader (code_loader.c):
   little-endian fields, and a 32-bit PC-relative one that must reach its target. A relocation that cannot be written
   gives SNX_LINK_RANGE (its value does not fit), SNX_LINK_OUTSIDE (its field is not inside its section),
   SNX_LINK_UNSUPPORTED (a type the linkers do not know) or SNX_LINK_MISALIGNED (an AArch64 load or store offset that
   its access size does not divide) */
enum { SNX_LINK_RANGE = 1, SNX_LINK_OUTSIDE, SNX_LINK_UNSUPPORTED, SNX_LINK_MISALIGNED };
void snx_link_put32(unsigned char* p, uint32_t value);
void snx_link_put64(unsigned char* p, uint64_t value);
/* target - place into the signed 32-bit field at p: 0, or SNX_LINK_RANGE with nothing written when it does not fit */
int snx_link_rel32(unsigned char* p, uint64_t target, uint64_t place);

#endif
