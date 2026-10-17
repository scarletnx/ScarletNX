/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ELF64 x86-64 and AArch64 relocatable objects, as LLVM and Clang write them: what the AOT linker (link_elf.c) and
   the JIT loader (code_loader.c) both do with one. Both take the same sections, resolve symbols by the same rules and
   write every relocation through snx_elf_relocate (and an AArch64 branch beyond B's reach through snx_elf_veneer), so
   code loaded into the process runs as the same code linked into a module. One set of objects is one machine: the
   two machines' relocation numbers mean different things (AArch64's ILP32 ones overlap x86-64's), so every function
   that reads a type takes the machine too. Not on Windows hosts, which have no <elf.h> and whose objects are COFF
   (link_coff.c) */
#ifndef SNX_RECOMPILER_LLVM_ELF_OBJECT_H
#define SNX_RECOMPILER_LLVM_ELF_OBJECT_H
#ifndef _WIN32
#include "common/cpu/recompiler/llvm/link.h"
#include <elf.h>
#include <stdint.h>

/* names newer than some systems' <elf.h> (FreeBSD's calls the unwind section SHT_AMD64_UNWIND) */
#ifndef SHT_X86_64_UNWIND
#define SHT_X86_64_UNWIND 0x70000001
#endif
#ifndef R_X86_64_GOTPCRELX
#define R_X86_64_GOTPCRELX 41
#endif
#ifndef R_X86_64_REX_GOTPCRELX
#define R_X86_64_REX_GOTPCRELX 42
#endif
#ifndef R_X86_64_PLT32
#define R_X86_64_PLT32 4
#endif
#ifndef EM_AARCH64
#define EM_AARCH64 183
#endif
/* AArch64's relocations, by number: not every <elf.h> has them (ELF for the Arm 64-bit Architecture, 5.7) */
enum {
    SNX_A64_NONE = 0,
    SNX_A64_ABS64 = 257,
    SNX_A64_PREL64 = 260,
    SNX_A64_PREL32 = 261,
    SNX_A64_ADR_PREL_LO21 = 274,
    SNX_A64_ADR_PREL_PG_HI21 = 275,
    SNX_A64_ADR_PREL_PG_HI21_NC = 276,
    SNX_A64_ADD_ABS_LO12_NC = 277,
    SNX_A64_LDST8_ABS_LO12_NC = 278,
    SNX_A64_TSTBR14 = 279,
    SNX_A64_CONDBR19 = 280,
    SNX_A64_JUMP26 = 282,
    SNX_A64_CALL26 = 283,
    SNX_A64_LDST16_ABS_LO12_NC = 284,
    SNX_A64_LDST32_ABS_LO12_NC = 285,
    SNX_A64_LDST64_ABS_LO12_NC = 286,
    SNX_A64_LDST128_ABS_LO12_NC = 299,
    SNX_A64_ADR_GOT_PAGE = 311,
    SNX_A64_LD64_GOT_LO12_NC = 312,
    SNX_A64_RELATIVE = 1027
};

/* where an allocated section goes, by its flags: the order of the AOT module's segments */
enum { SNX_ELF_RODATA, SNX_ELF_TEXT, SNX_ELF_DATA, SNX_ELF_BSS, SNX_ELF_KINDS };

/* A view of one input object. The section arrays are one block, freed by snx_elf_free */
typedef struct {
    const SnxLinkInput* input;
    const Elf64_Ehdr* eh;
    const Elf64_Shdr* sh;
    const Elf64_Sym* syms;
    size_t nsyms;
    const char* strtab;
    int* kind;          /* per section: SNX_ELF_*, -1 when not loaded */
    uint64_t* address;  /* per section: final address */
    uint64_t* position; /* per section: offset inside its kind */
    /* the object's own veneers (snx_elf_veneer), right after its code where all of it reaches them: their count,
       which the caller sets before snx_elf_layout, and the island's offset in the code, which it computes. The AOT
       linker gives each object an island; the JIT loader keeps one for the whole load */
    size_t veneers;
    uint64_t island;
} SnxElfObject;

/* Checks the object and classifies its sections. *machine is the set's (EM_X86_64 or EM_AARCH64): 0 takes the
   object's, anything else refuses another machine's object. 0, or -1 with the reason in error */
int snx_elf_parse(SnxElfObject* o, const SnxLinkInput* input, unsigned* machine, char* error, size_t error_size);
void snx_elf_free(SnxElfObject* o);
const char* snx_elf_symbol_name(const SnxElfObject* o, const Elf64_Sym* s);
const char* snx_elf_section_name(const SnxElfObject* o, size_t index);

/* Every global and weak definition of the set into globals, name -> (object << 32 | symbol index). A strong definition
   wins over weak ones, two strong ones are an error. Names in loaded (NULL for none: symbols outside the set, already
   defined) are skipped when the definition is weak and an error when it is strong */
int snx_elf_collect_globals(const SnxElfObject* objs, size_t count, const SnxLinkNames* loaded, SnxLinkNames* globals,
                            char* error, size_t error_size);

/* Where object's symbol index is defined: SNX_ELF_HERE, with the defining object and symbol in the set (itself for a
   local or defined symbol), or SNX_ELF_ELSEWHERE for an undefined one no object of the set defines, which the caller
   resolves by its name or reports. -1 for a bad index */
enum { SNX_ELF_HERE, SNX_ELF_ELSEWHERE };
int snx_elf_definition(const SnxElfObject* objs, const SnxLinkNames* globals, size_t object, size_t index,
                       size_t* def_object, size_t* def_index, char* error, size_t error_size);

/* The address of a definition in the set once its sections have addresses; *absolute for an SHN_ABS value */
uint64_t snx_elf_symbol_address(const SnxElfObject* objs, size_t object, size_t index, int* absolute);

/* 1 for the relocations that go through a GOT slot */
int snx_elf_got_relocation(unsigned machine, unsigned type);
/* 1 for a call or jump that may go through a stub or veneer when its target is out of its reach: x86-64's PLT32
   (rel32, so only a target outside the 2 GiB the code is in), AArch64's CALL26 and JUMP26 (B and BL: 128 MiB) */
int snx_elf_call_relocation(unsigned machine, unsigned type);
/* 1 for the 64-bit absolute address, which a position-independent module rebases with the machine's R_*_RELATIVE */
int snx_elf_absolute_relocation(unsigned machine, unsigned type);
/* R_X86_64_RELATIVE or R_AARCH64_RELATIVE */
unsigned snx_elf_relative_type(unsigned machine);

/* Positions of the loaded sections inside their kinds, in object and section order, and each kind's size and
   alignment; an object with veneers gets its island right after its code */
void snx_elf_layout(SnxElfObject* objs, size_t count, uint64_t* kind_size, uint64_t* kind_align);

/* Writes relocation type of machine at p, the place at address P with room bytes left in its section: S the symbol's
   address, A the addend, got the GOT's address and slot the address of the symbol's GOT slot (GOT relocations only).
   AArch64's instruction relocations patch the immediate of the instruction already there. 0, or SNX_LINK_OUTSIDE when
   the field does not fit in the section, SNX_LINK_RANGE when the value does not fit the field (an out-of-range B or BL
   goes through a veneer: snx_elf_veneer), SNX_LINK_MISALIGNED when a scaled load or store offset is not a multiple of
   its access size, SNX_LINK_UNSUPPORTED for a type neither linker handles; nothing is written then */
int snx_elf_relocate(unsigned machine, unsigned char* p, size_t room, unsigned type, uint64_t S, int64_t A, uint64_t P,
                     uint64_t got, uint64_t slot);

/* An AArch64 veneer, what B and BL reach a target beyond 128 MiB through: three instructions, x16 their scratch
   register (the procedure call standard's IP0, which a call may clobber), within ADRP's 4 GiB of the target. Direct:
   ADRP x16, target; ADD x16, x16, :lo12:target; BR x16. Indirect: the same with LDR x16, [x16, :lo12:target] in place
   of the ADD, jumping to the address held in the 8-byte slot at target (a GOT slot), for a target anywhere */
#define SNX_ELF_VENEER_SIZE 12
/* Writes at v the veneer at address at. 0, or SNX_LINK_RANGE beyond ADRP's reach, SNX_LINK_MISALIGNED for an indirect
   slot not 8-aligned */
int snx_elf_veneer(unsigned char* v, uint64_t at, uint64_t target, int indirect);
/* An AArch64 CALL26 or JUMP26 at p (address P) that cannot reach target: the veneer at v (address at) written to
   jump there, and the branch to the veneer (both types patch the same field). The caller provides the veneer, which
   must be in the branch's reach: 0, or what snx_elf_veneer and snx_elf_relocate return */
int snx_elf_branch_veneer(unsigned char* p, uint64_t P, unsigned char* v, uint64_t at, uint64_t target, int indirect);

#endif
#endif
