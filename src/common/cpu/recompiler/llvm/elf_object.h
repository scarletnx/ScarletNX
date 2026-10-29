/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ELF64 x86-64, AArch64, PowerPC64, RISC-V and LoongArch relocatable objects, as LLVM and Clang write them: what the
   AOT linker (link_elf.c) and the JIT loader (code_loader.c) both do with one. Both take the same sections, resolve
   symbols by the same rules and write every relocation through snx_elf_relocate (and an AArch64 branch beyond B's reach
   through snx_elf_veneer), so code loaded into the process runs as the same code linked into a module. One set of
   objects is one machine: the machines' relocation numbers mean different things (AArch64's ILP32 ones and the others'
   overlap x86-64's), so every function that reads a type takes the machine too. The objects are in the host's byte
   order, as they are linked or loaded where they run. Only the AOT linker takes PowerPC64's, RISC-V's and LoongArch's
   (code_loader.h). Not on Windows,
   whose objects are COFF (link_coff.c, code_loader_coff.c). Darwin has no <elf.h>: its definitions are elf_defs.h's,
   for the JIT loader's ELF objects there (code_loader.c) */
#ifndef SNX_RECOMPILER_LLVM_ELF_OBJECT_H
#define SNX_RECOMPILER_LLVM_ELF_OBJECT_H
#if !defined(_WIN32)
#include "common/cpu/recompiler/llvm/link.h"
#ifdef __APPLE__
#include "common/cpu/recompiler/llvm/elf_defs.h"
#else
#include <elf.h>
#endif
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
#ifndef EM_PPC64
#define EM_PPC64 21
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
/* PowerPC64's, by number (glibc's <elf.h> defines most of them as PowerPC's 32-bit names) */
enum {
    SNX_PPC_NONE = 0,
    SNX_PPC_ADDR32 = 1,
    SNX_PPC_ADDR16_LO = 4,
    SNX_PPC_ADDR16_HI = 5,
    SNX_PPC_ADDR16_HA = 6,
    SNX_PPC_REL24 = 10,
    SNX_PPC_REL14 = 11,
    SNX_PPC_RELATIVE = 22,
    SNX_PPC_REL32 = 26,
    SNX_PPC_ADDR64 = 38,
    SNX_PPC_REL64 = 44,
    SNX_PPC_TOC16 = 47,
    SNX_PPC_TOC16_LO = 48,
    SNX_PPC_TOC16_HI = 49,
    SNX_PPC_TOC16_HA = 50,
    SNX_PPC_TOC = 51,
    SNX_PPC_ADDR16_LO_DS = 57,
    SNX_PPC_TOC16_DS = 63,
    SNX_PPC_TOC16_LO_DS = 64,
    SNX_PPC_ENTRY = 118,
    SNX_PPC_PLTSEQ = 119,
    SNX_PPC_PLTCALL = 120,
    SNX_PPC_REL16 = 249,
    SNX_PPC_REL16_LO = 250,
    SNX_PPC_REL16_HI = 251,
    SNX_PPC_REL16_HA = 252
};
#ifndef EM_RISCV
#define EM_RISCV 243
#endif
#ifndef EM_LOONGARCH
#define EM_LOONGARCH 258
#endif
/* RISC-V's, by number (the RISC-V ELF psABI). The objects are made without linker relaxation (ir.c), so a RELAX or an
   ALIGN never asks for code to move */
enum {
    SNX_RV_NONE = 0,
    SNX_RV_32 = 1,
    SNX_RV_64 = 2,
    SNX_RV_RELATIVE = 3,
    SNX_RV_BRANCH = 16,
    SNX_RV_JAL = 17,
    SNX_RV_CALL = 18,
    SNX_RV_CALL_PLT = 19,
    SNX_RV_GOT_HI20 = 20,
    SNX_RV_PCREL_HI20 = 23,
    SNX_RV_PCREL_LO12_I = 24,
    SNX_RV_PCREL_LO12_S = 25,
    SNX_RV_ADD8 = 33,
    SNX_RV_ADD16 = 34,
    SNX_RV_ADD32 = 35,
    SNX_RV_ADD64 = 36,
    SNX_RV_SUB8 = 37,
    SNX_RV_SUB16 = 38,
    SNX_RV_SUB32 = 39,
    SNX_RV_SUB64 = 40,
    SNX_RV_ALIGN = 43,
    SNX_RV_RVC_BRANCH = 44,
    SNX_RV_RVC_JUMP = 45,
    SNX_RV_RELAX = 51,
    SNX_RV_SUB6 = 52,
    SNX_RV_SET6 = 53,
    SNX_RV_SET8 = 54,
    SNX_RV_SET16 = 55,
    SNX_RV_SET32 = 56,
    SNX_RV_32_PCREL = 57,
    SNX_RV_PLT32 = 59
};
/* LoongArch's, by number (the LoongArch ELF psABI), likewise made without relaxation */
enum {
    SNX_LA_NONE = 0,
    SNX_LA_32 = 1,
    SNX_LA_64 = 2,
    SNX_LA_RELATIVE = 3,
    SNX_LA_MARK_LA = 20,
    SNX_LA_MARK_PCREL = 21,
    SNX_LA_ADD8 = 47,
    SNX_LA_ADD16 = 48,
    SNX_LA_ADD24 = 49,
    SNX_LA_ADD32 = 50,
    SNX_LA_ADD64 = 51,
    SNX_LA_SUB8 = 52,
    SNX_LA_SUB16 = 53,
    SNX_LA_SUB24 = 54,
    SNX_LA_SUB32 = 55,
    SNX_LA_SUB64 = 56,
    SNX_LA_B16 = 64,
    SNX_LA_B21 = 65,
    SNX_LA_B26 = 66,
    SNX_LA_PCALA_HI20 = 71,
    SNX_LA_PCALA_LO12 = 72,
    SNX_LA_GOT_PC_HI20 = 75,
    SNX_LA_GOT_PC_LO12 = 76,
    SNX_LA_32_PCREL = 99,
    SNX_LA_RELAX = 100,
    SNX_LA_ALIGN = 102,
    SNX_LA_PCREL20_S2 = 103,
    SNX_LA_ADD6 = 105,
    SNX_LA_SUB6 = 106,
    SNX_LA_64_PCREL = 109,
    SNX_LA_CALL36 = 110
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
       linker and the JIT loader give each object an island once the code passes SNX_ELF_VENEER_TEXT; below it the
       loader keeps one stub area for the whole load */
    size_t veneers;
    uint64_t island;
} SnxElfObject;

/* Checks the object and classifies its sections. *machine is the set's (EM_X86_64, EM_AARCH64, EM_PPC64, EM_RISCV or
   EM_LOONGARCH): 0 takes the object's, anything else refuses another machine's object. *abi likewise: PowerPC64's ABI
   as the objects' e_flags say (snx_elf_ppc_abi), RISC-V's and LoongArch's e_flags whole (their float ABI, which the
   module's own e_flags repeat for the loader), 0 for the other machines. 0, or -1 with the reason in error */
int snx_elf_parse(SnxElfObject* o, const SnxLinkInput* input, unsigned* machine, unsigned* abi, char* error,
                  size_t error_size);
/* PowerPC64's ABI from an object's e_flags: SNX_ELF_PPC_ELFV2 (ppc64le, and big-endian musl), or SNX_ELF_PPC_ELFV1
   (big-endian glibc, 0 or 1 in e_flags), whose function symbols name .opd descriptors (entry, TOC, environment) rather
   than code */
enum { SNX_ELF_PPC_ELFV1 = 1, SNX_ELF_PPC_ELFV2 = 2 };
unsigned snx_elf_ppc_abi(unsigned e_flags);
/* the host's byte order, ELFDATA2LSB or ELFDATA2MSB: the objects' and the module's */
unsigned char snx_elf_host_data(void);
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
   (rel32, so only a target outside the 2 GiB the code is in), AArch64's CALL26 and JUMP26 (B and BL: 128 MiB),
   PowerPC64's REL24 (b and bl: 32 MiB, and its target is an entry point: snx_elf_ppc_entry) */
int snx_elf_call_relocation(unsigned machine, unsigned type);
/* 1 for the 64-bit absolute address, which a position-independent module rebases with the machine's R_*_RELATIVE.
   PowerPC64's R_PPC64_TOC is one too: the TOC's address, in an ELFv1 .opd descriptor */
int snx_elf_absolute_relocation(unsigned machine, unsigned type);
/* R_X86_64_RELATIVE, R_AARCH64_RELATIVE, R_PPC64_RELATIVE, R_RISCV_RELATIVE or R_LARCH_RELATIVE */
unsigned snx_elf_relative_type(unsigned machine);
/* RISC-V's low halves, R_RISCV_PCREL_LO12_I and _S: their symbol is the label of the AUIPC whose HI20 relocation
   (PCREL_HI20 or GOT_HI20) they complete, and their value that relocation's own displacement. 1 for them */
int snx_elf_riscv_lo12(unsigned machine, unsigned type);
/* The index in rela (n entries, of the section the label is in) of the HI20 relocation at offset, the label's place
   in its section; -1 for none */
long snx_elf_riscv_hi20(const Elf64_Rela* rela, size_t n, uint64_t offset);

/* One section of the set: objs[object].sh[section] */
typedef struct {
    size_t object, section;
} SnxElfPlace;
/* Positions of the loaded sections inside their kinds, in object and section order, and each kind's size and
   alignment; an object with veneers gets its island right after its code, of its machine's veneers (AArch64's
   SNX_ELF_VENEER_SIZE, PowerPC64's SNX_ELF_PPC_VENEER_SIZE). text (NULL for that order) is every code section of the
   set in the order the code takes instead: only for objects without veneers, whose islands go after their own code */
void snx_elf_layout(SnxElfObject* objs, size_t count, const SnxElfPlace* text, uint64_t* kind_size,
                    uint64_t* kind_align);
/* The objects' code with each section's alignment added, no less than what snx_elf_layout places before islands */
uint64_t snx_elf_text_size(const SnxElfObject* objs, size_t count);
/* Below this much code (snx_elf_text_size) every AArch64 B and BL of a module or load reaches its target (128 MiB, with
   room for the islands and stubs), and no object needs veneers of its own */
#define SNX_ELF_VENEER_TEXT ((uint64_t)120 << 20)

/* Writes relocation type of machine at p, the place at address P with room bytes left in its section: S the symbol's
   address, A the addend, got the GOT's address and slot the address of the symbol's GOT slot (GOT relocations only).
   On PowerPC64, which has no GOT relocations, got is the TOC's address (.TOC.), which the TOC16 relocations count
   from and R_PPC64_TOC writes, and a REL24's S + A is the entry point the branch goes to (snx_elf_ppc_entry). For
   RISC-V's PCREL_LO12_I and _S, S is the displacement of the HI20 relocation they complete (snx_elf_riscv_hi20) and A
   must be 0. The instruction relocations patch the immediate of the instruction already there. 0, or
   SNX_LINK_OUTSIDE when the field does not fit in the section, SNX_LINK_RANGE when the value does not fit the field
   (an out-of-range B or BL goes through a veneer: snx_elf_veneer, snx_elf_ppc_veneer), SNX_LINK_MISALIGNED when a
   scaled load or store offset is not a multiple of its access size, SNX_LINK_UNSUPPORTED for a type neither linker
   handles; nothing is written then */
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

/* Where a PowerPC64 call to the definition (def_object, def_index) at S, plus A, enters. ELFv1: a function symbol
   names its .opd descriptor, whose first doubleword is the code's address, relocated by the R_PPC64_ADDR64 at the
   descriptor: *global and *local are that address. ELFv2: *global is S + A, and *local is past the global entry's
   two instructions that compute r2 from r12 (st_other's bits 5-7, the ABI's PPC64_LOCAL_ENTRY_OFFSET), where a call
   from code sharing the TOC enters. A call to a function that doesn't keep r2 (PC-relative code) is refused, as the
   caller expects its TOC back. 0, or -1 with the reason in error */
int snx_elf_ppc_entry(const SnxElfObject* objs, const SnxLinkNames* globals, unsigned abi, size_t def_object,
                      size_t def_index, uint64_t S, int64_t A, uint64_t* global, uint64_t* local, char* error,
                      size_t error_size);
/* A PowerPC64 veneer, what bl reaches a target beyond 32 MiB through, within 2 GiB of the TOC: r12 = the target, from
   r2 (the TOC, the same everywhere in the module), then a jump through the count register. ELFv2's global entry
   expects its address in r12, as from a pointer, so the target is the global entry */
#define SNX_ELF_PPC_VENEER_SIZE 16
/* A REL24 at p (address P) that cannot reach target: the veneer at v (address at) written to jump there, through
   toc, and the branch to the veneer. 0, or SNX_LINK_RANGE when the veneer is out of the branch's reach or the target
   out of the TOC's */
int snx_elf_ppc_branch_veneer(unsigned char* p, uint64_t P, unsigned char* v, uint64_t at, uint64_t target,
                              uint64_t toc);

#endif
#endif
