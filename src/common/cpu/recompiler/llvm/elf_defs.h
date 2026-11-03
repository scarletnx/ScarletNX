/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The part of <elf.h> that elf_object.c and its users read relocatable objects with, for systems that have no <elf.h>:
   Darwin, where the JIT loader (code_loader.c) loads ELF objects although the system's own are Mach-O, and the
   Android bridge's linker on every host (it loads AArch64 shared objects, whatever the host's own format). Written from
   the System V ABI's ELF chapters (generic ABI 4.1: the header, sections, symbols, relocations) and the processor
   supplements' relocation numbers (x86-64 psABI 4.4, ELF for the Arm 64-bit Architecture 5.7, 64-bit ELF V2 ABI for
   Power 3.5). Only the names those files use: elf_object.h defines the AArch64 relocations it writes itself */
#ifndef SNX_RECOMPILER_LLVM_ELF_DEFS_H
#define SNX_RECOMPILER_LLVM_ELF_DEFS_H
#include <stdint.h>

typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef uint64_t Elf64_Xword;
typedef int64_t Elf64_Sxword;
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Section;

/* e_ident */
#define EI_NIDENT   16
#define EI_CLASS    4
#define EI_DATA     5
#define ELFMAG      "\177ELF"
#define SELFMAG     4
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off e_phoff;
    Elf64_Off e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
} Elf64_Ehdr;

/* e_type, e_machine */
#define ET_REL     1
#define ET_DYN     3
#define EM_PPC64   21
#define EM_X86_64  62
#define EM_AARCH64 183

typedef struct {
    Elf64_Word sh_name;
    Elf64_Word sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr sh_addr;
    Elf64_Off sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word sh_link;
    Elf64_Word sh_info;
    Elf64_Xword sh_addralign;
    Elf64_Xword sh_entsize;
} Elf64_Shdr;

/* sh_type, sh_flags and the reserved section indexes */
#define SHT_PROGBITS      1
#define SHT_SYMTAB        2
#define SHT_RELA          4
#define SHT_NOTE          7
#define SHT_NOBITS        8
#define SHT_REL           9
#define SHT_DYNSYM        11
#define SHT_INIT_ARRAY    14
#define SHT_FINI_ARRAY    15
#define SHT_PREINIT_ARRAY 16
#define SHT_GROUP         17
#define SHT_X86_64_UNWIND 0x70000001
#define SHF_WRITE         0x1
#define SHF_ALLOC         0x2
#define SHF_EXECINSTR     0x4
#define SHF_TLS           0x400
#define SHN_UNDEF         0
#define SHN_ABS           0xfff1
#define SHN_COMMON        0xfff2

typedef struct {
    Elf64_Word st_name;
    unsigned char st_info; /* binding << 4 | type */
    unsigned char st_other;
    Elf64_Section st_shndx;
    Elf64_Addr st_value;
    Elf64_Xword st_size;
} Elf64_Sym;

#define ELF64_ST_BIND(info) ((info) >> 4)
#define ELF64_ST_TYPE(info) ((info) & 0xf)
#define STB_GLOBAL          1
#define STB_WEAK            2
#define STT_SECTION         3

typedef struct {
    Elf64_Addr r_offset;
    Elf64_Xword r_info; /* symbol << 32 | type */
    Elf64_Sxword r_addend;
} Elf64_Rela;

#define ELF64_R_SYM(info)       ((info) >> 32)
#define ELF64_R_TYPE(info)      ((info) & 0xffffffffu)
#define ELF64_R_INFO(sym, type) (((Elf64_Xword)(sym) << 32) | (Elf64_Xword)(type))

/* x86-64 */
#define R_X86_64_NONE          0
#define R_X86_64_64            1
#define R_X86_64_PC32          2
#define R_X86_64_PLT32         4
#define R_X86_64_RELATIVE      8
#define R_X86_64_GOTPCREL      9
#define R_X86_64_PC64          24
#define R_X86_64_GOTOFF64      25
#define R_X86_64_GOTPC32       26
#define R_X86_64_GOTPC64       29
#define R_X86_64_GOTPCRELX     41
#define R_X86_64_REX_GOTPCRELX 42
/* AArch64 (the rest: elf_object.h's SNX_A64_ names) */
#define R_AARCH64_NONE     0
#define R_AARCH64_RELATIVE 1027
/* PowerPC64 */
#define R_PPC64_NONE      0
#define R_PPC64_RELATIVE  22
#define R_PPC64_ADDR64    38
#define R_PPC64_TOC       51

/* the program headers and the dynamic section, for a loader of shared objects (the Android bridge's linker) */
typedef struct {
    Elf64_Word p_type;
    Elf64_Word p_flags;
    Elf64_Off p_offset;
    Elf64_Addr p_vaddr;
    Elf64_Addr p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
} Elf64_Phdr;
#define PT_LOAD      1
#define PT_DYNAMIC   2
#define PT_PHDR      6
#define PT_TLS       7
#define PT_GNU_RELRO 0x6474e552
#define PF_X         0x1
#define PF_W         0x2
#define PF_R         0x4

typedef struct {
    Elf64_Sxword d_tag;
    union {
        Elf64_Xword d_val;
        Elf64_Addr d_ptr;
    } d_un;
} Elf64_Dyn;
#define DT_NULL            0
#define DT_NEEDED          1
#define DT_PLTRELSZ        2
#define DT_HASH            4
#define DT_STRTAB          5
#define DT_SYMTAB          6
#define DT_RELA            7
#define DT_RELASZ          8
#define DT_STRSZ           10
#define DT_INIT            12
#define DT_FINI            13
#define DT_SONAME          14
#define DT_SYMBOLIC        16
#define DT_JMPREL          23
#define DT_INIT_ARRAY      25
#define DT_FINI_ARRAY      26
#define DT_INIT_ARRAYSZ    27
#define DT_FINI_ARRAYSZ    28
#define DT_FLAGS           30
#define DT_PREINIT_ARRAY   32
#define DT_PREINIT_ARRAYSZ 33
#define DT_RELRSZ          35
#define DT_RELR            36
#define DT_GNU_HASH        0x6ffffef5
#define DF_SYMBOLIC        0x2

#define STB_LOCAL  0
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC   2
#define STT_TLS    6
#define STT_GNU_IFUNC 10

/* AArch64's dynamic relocations (ELF for the Arm 64-bit Architecture 5.7.10) */
#define R_AARCH64_ABS64     257
#define R_AARCH64_GLOB_DAT  1025
#define R_AARCH64_JUMP_SLOT 1026
#define R_AARCH64_TLS_DTPMOD64 1028
#define R_AARCH64_TLS_DTPREL64 1029
#define R_AARCH64_TLS_TPREL64  1030
#define R_AARCH64_TLSDESC      1031
#define R_AARCH64_IRELATIVE 1032

/* the generic ABI's sizes, which the objects' bytes are read with */
typedef char snx_elf64_ehdr_size[sizeof(Elf64_Ehdr) == 64 ? 1 : -1];
typedef char snx_elf64_shdr_size[sizeof(Elf64_Shdr) == 64 ? 1 : -1];
typedef char snx_elf64_sym_size[sizeof(Elf64_Sym) == 24 ? 1 : -1];
typedef char snx_elf64_rela_size[sizeof(Elf64_Rela) == 24 ? 1 : -1];
typedef char snx_elf64_phdr_size[sizeof(Elf64_Phdr) == 56 ? 1 : -1];
typedef char snx_elf64_dyn_size[sizeof(Elf64_Dyn) == 16 ? 1 : -1];

#endif
