/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The linker: a 64-bit program's modules relocated as the console's own linker (the rtld module each game carries)
   relocates them, from their dynamic tables as they lie mapped. Written from the ELF and AArch64 ABI specifications
   (the dynamic section, RELA and DT_RELR relocations) and from what a game's rtld is seen to do with them.

   A relative relocation (R_AARCH64_RELATIVE of DT_RELA, and DT_RELR's, whose addend is the word itself) takes the
   module's base. A relocation with a symbol (R_AARCH64_ABS64 and GLOB_DAT of DT_RELA, JUMP_SLOT of DT_JMPREL) takes
   the symbol's address and the addend: the first definition of its name among the modules in their order, the load
   order, that is not local, and for a symbol whose visibility is not the default the module's own. A definition is a
   symbol with a section, not the common one, and not a file's. A weak symbol nothing defines is 0; any other one
   nothing defines leaves its word as it was, a JUMP_SLOT its lazy value: the word as loaded plus the base, the
   address of the code that binds the slot at its first call */
#ifndef SNX_NX_RELOCATE_H
#define SNX_NX_RELOCATE_H
#include "common/utilities/common.h"
#include <stddef.h>

/* One module to link. image is the module as mapped at base, size bytes of it readable: its tables are read there.
   data is where the words of [data_off, data_off + data_size) are relocated: image + data_off to link the module in
   place, or a copy of those bytes, which leaves the module as it is. snx_relocate sets the rest: lazy, what a PLT
   slot of the module holds until it is bound, 0 for a module with none; init and fini, the addresses of the functions
   its DT_INIT and DT_FINI name, which run its constructors and destructors, 0 for none */
typedef struct {
    const char* name; /* in messages */
    const unsigned char* image;
    SnxU64 base, size;
    unsigned char* data;
    SnxU64 data_off, data_size;
    SnxU64 lazy, init, fini;
} SnxRelocModule;

/* What snx_relocate did: the relocations applied by kind, those of a weak symbol nothing defines (0), those of another
   symbol nothing defines (as they were) with the first one's name (in its module's image), and the ones it left
   alone: a target outside its module's data, or a type the console's linker has no case for */
typedef struct {
    SnxU64 relative, symbolic, slots;
    SnxU64 absent, unresolved, outside, other;
    const char* first_unresolved;
} SnxRelocCounts;

/* Relocates each module's data. 0, or -1 said in error for a table that lies outside its module's image */
int snx_relocate(SnxRelocModule* modules, size_t count, SnxRelocCounts* counts, char* error, size_t error_size);

/* How modules' images differ from their data at the words that are relocated, for modules snx_relocate linked into a
   copy: words the relocated words compared, lazy the PLT slots the image still holds the lazy value of (not bound
   yet, which is no difference), differ the ones that hold anything else than the copy, the first of them in module
   `module` at offset `at`, where the image has `has` and the copy `wants` */
typedef struct {
    SnxU64 words, lazy, differ;
    size_t module;
    SnxU64 at, has, wants;
} SnxRelocDiff;
int snx_relocate_compare(const SnxRelocModule* modules, size_t count, SnxRelocDiff* diff, char* error,
                         size_t error_size);

/* The address a symbol of that name is bound to among the modules, by the rule above for a symbol of default
   visibility: 1 with it in *address, 0 when nothing defines the name, -1 said in error for a table outside its
   module */
int snx_relocate_find(const SnxRelocModule* modules, size_t count, const char* name, SnxU64* address, char* error,
                      size_t error_size);

#endif
