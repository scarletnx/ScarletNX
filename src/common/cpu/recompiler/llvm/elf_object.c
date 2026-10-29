/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ELF64 x86-64, AArch64, PowerPC64, RISC-V and LoongArch relocatable objects: parsing, symbol resolution, relocation
   and the AArch64 and PowerPC64 veneers for link_elf.c and code_loader.c */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L /* snprintf */
#include "common/cpu/recompiler/llvm/elf_object.h"
#include "common/cpu/recompiler/llvm/link_internal.h"
#include "common/utilities/bits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char* snx_elf_symbol_name(const SnxElfObject* o, const Elf64_Sym* s) {
    if (ELF64_ST_TYPE(s->st_info) == STT_SECTION) return "<section>";
    return o->strtab + s->st_name;
}

const char* snx_elf_section_name(const SnxElfObject* o, size_t index) {
    return (const char*)o->input->data + o->sh[o->eh->e_shstrndx].sh_offset + o->sh[index].sh_name;
}

unsigned snx_elf_ppc_abi(unsigned e_flags) { return (e_flags & 3) == 2 ? SNX_ELF_PPC_ELFV2 : SNX_ELF_PPC_ELFV1; }

unsigned char snx_elf_host_data(void) {
    const uint16_t one = 1;
    unsigned char first;
    memcpy(&first, &one, 1);
    return first ? ELFDATA2LSB : ELFDATA2MSB;
}

int snx_elf_parse(SnxElfObject* o, const SnxLinkInput* input, unsigned* machine, unsigned* abi, char* error,
                  size_t error_size) {
    *o = (SnxElfObject){0};
    o->input = input;
    o->eh = (const Elf64_Ehdr*)input->data;
    if (input->size < sizeof(Elf64_Ehdr) || memcmp(o->eh->e_ident, ELFMAG, SELFMAG) ||
        o->eh->e_ident[EI_CLASS] != ELFCLASS64 || o->eh->e_ident[EI_DATA] != snx_elf_host_data() ||
        o->eh->e_type != ET_REL ||
        (o->eh->e_machine != EM_X86_64 && o->eh->e_machine != EM_AARCH64 && o->eh->e_machine != EM_PPC64 &&
         o->eh->e_machine != EM_RISCV && o->eh->e_machine != EM_LOONGARCH) ||
        o->eh->e_shentsize != sizeof(Elf64_Shdr) ||
        o->eh->e_shoff + (uint64_t)o->eh->e_shnum * sizeof(Elf64_Shdr) > input->size)
        return snx_link_fail(
            error, error_size,
            "%s: not an x86-64, AArch64, PowerPC64, RISC-V or LoongArch ELF relocatable object in this host's "
            "byte order%s",
            input->name, "");
    unsigned object_abi = o->eh->e_machine == EM_PPC64 ? snx_elf_ppc_abi(o->eh->e_flags)
                          : o->eh->e_machine == EM_RISCV || o->eh->e_machine == EM_LOONGARCH
                              ? o->eh->e_flags
                              : 0;
    if (!*machine) {
        *machine = o->eh->e_machine;
        *abi = object_abi;
    }
    if (o->eh->e_machine != *machine)
        return snx_link_fail(error, error_size, "%s: another machine's object%s", input->name, "");
    if (object_abi != *abi)
        return snx_link_fail(error, error_size, "%s: another ABI's object (%s)", input->name,
                             *machine == EM_PPC64 ? "ELFv1 and ELFv2"
                                                  : "e_flags differ: the float ABI, or compressed code");
    o->sh = (const Elf64_Shdr*)(input->data + o->eh->e_shoff);
    /* one block, the 8-byte arrays first */
    o->address = (uint64_t*)calloc(o->eh->e_shnum, 2 * sizeof(uint64_t) + sizeof(int));
    if (!o->address) return snx_link_fail(error, error_size, "out of memory%s%s", "", "");
    o->position = o->address + o->eh->e_shnum;
    o->kind = (int*)(o->position + o->eh->e_shnum);
    for (size_t i = 0; i < o->eh->e_shnum; ++i) {
        const Elf64_Shdr* s = &o->sh[i];
        o->kind[i] = -1;
        if (s->sh_type != SHT_NOBITS && s->sh_offset + s->sh_size > input->size)
            return snx_link_fail(error, error_size, "%s: truncated section%s", input->name, "");
        if (s->sh_type == SHT_SYMTAB) {
            o->syms = (const Elf64_Sym*)(input->data + s->sh_offset);
            o->nsyms = s->sh_size / sizeof(Elf64_Sym);
            if (s->sh_link >= o->eh->e_shnum)
                return snx_link_fail(error, error_size, "%s: bad symbol table%s", input->name, "");
            o->strtab = (const char*)input->data + o->sh[s->sh_link].sh_offset;
        }
        if (!(s->sh_flags & SHF_ALLOC)) continue;
        if (s->sh_flags & SHF_TLS)
            return snx_link_fail(error, error_size, "%s: thread-local data is not supported%s", input->name, "");
        if (s->sh_type == SHT_INIT_ARRAY || s->sh_type == SHT_FINI_ARRAY || s->sh_type == SHT_PREINIT_ARRAY)
            return snx_link_fail(error, error_size, "%s: constructors are not supported%s", input->name, "");
        if (s->sh_type == SHT_NOBITS)
            o->kind[i] = SNX_ELF_BSS;
        else if (s->sh_type == SHT_PROGBITS || s->sh_type == SHT_X86_64_UNWIND)
            o->kind[i] = s->sh_flags & SHF_EXECINSTR ? SNX_ELF_TEXT
                         : s->sh_flags & SHF_WRITE   ? SNX_ELF_DATA
                                                     : SNX_ELF_RODATA;
        else if (s->sh_type == SHT_NOTE || s->sh_type == SHT_GROUP)
            continue;
        else
            return snx_link_fail(error, error_size, "%s: unsupported allocated section type%s", input->name, "");
    }
    if (!o->syms) return snx_link_fail(error, error_size, "%s: no symbol table%s", input->name, "");
    return 0;
}

void snx_elf_free(SnxElfObject* o) {
    free(o->address);
    o->address = o->position = NULL;
    o->kind = NULL;
}

int snx_elf_collect_globals(const SnxElfObject* objs, size_t count, const SnxLinkNames* loaded, SnxLinkNames* globals,
                            char* error, size_t error_size) {
    size_t found;
    for (size_t i = 0; i < count; ++i) {
        const SnxElfObject* o = &objs[i];
        for (size_t j = 1; j < o->nsyms; ++j) {
            const Elf64_Sym* s = &o->syms[j];
            int bind = ELF64_ST_BIND(s->st_info);
            if ((bind != STB_GLOBAL && bind != STB_WEAK) || s->st_shndx == SHN_UNDEF) continue;
            const char* name = snx_elf_symbol_name(o, s);
            if (s->st_shndx == SHN_COMMON)
                return snx_link_fail(error, error_size, "%s: common symbol %s", o->input->name, name);
            if (loaded && snx_link_find(loaded, name, &found)) {
                if (bind == STB_WEAK) continue;
                return snx_link_fail(error, error_size, "duplicate symbol %s (%s defines one already loaded)", name,
                                     o->input->name);
            }
            if (snx_link_find(globals, name, &found)) {
                const SnxElfObject* other = &objs[found >> 32];
                if (bind == STB_WEAK) continue;
                if (ELF64_ST_BIND(other->syms[found & 0xFFFFFFFFu].st_info) != STB_WEAK)
                    return snx_link_fail(error, error_size, "duplicate symbol %s (%s)", name, o->input->name);
            }
            snx_link_insert(globals, name, (i << 32) | j);
        }
    }
    return 0;
}

int snx_elf_definition(const SnxElfObject* objs, const SnxLinkNames* globals, size_t object, size_t index,
                       size_t* def_object, size_t* def_index, char* error, size_t error_size) {
    const SnxElfObject* o = &objs[object];
    size_t found;
    if (index >= o->nsyms) return snx_link_fail(error, error_size, "%s: bad symbol index%s", o->input->name, "");
    const Elf64_Sym* s = &o->syms[index];
    if (s->st_shndx != SHN_UNDEF) {
        *def_object = object;
        *def_index = index;
        return SNX_ELF_HERE;
    }
    if (snx_link_find(globals, snx_elf_symbol_name(o, s), &found)) {
        *def_object = found >> 32;
        *def_index = found & 0xFFFFFFFFu;
        return SNX_ELF_HERE;
    }
    return SNX_ELF_ELSEWHERE;
}

uint64_t snx_elf_symbol_address(const SnxElfObject* objs, size_t object, size_t index, int* absolute) {
    const SnxElfObject* o = &objs[object];
    const Elf64_Sym* s = &o->syms[index];
    *absolute = s->st_shndx == SHN_ABS;
    if (*absolute) return s->st_value;
    return o->address[s->st_shndx] + s->st_value;
}

int snx_elf_got_relocation(unsigned machine, unsigned type) {
    if (machine == EM_PPC64) return 0; /* its objects keep their addresses in their own .toc sections */
    if (machine == EM_AARCH64) return type == SNX_A64_ADR_GOT_PAGE || type == SNX_A64_LD64_GOT_LO12_NC;
    /* RISC-V's low half names the AUIPC's label, not the symbol: the slot is its HI20's */
    if (machine == EM_RISCV) return type == SNX_RV_GOT_HI20;
    if (machine == EM_LOONGARCH) return type == SNX_LA_GOT_PC_HI20 || type == SNX_LA_GOT_PC_LO12;
    return type == R_X86_64_GOTPCREL || type == R_X86_64_GOTPCRELX || type == R_X86_64_REX_GOTPCRELX;
}

/* RISC-V's AUIPC and JALR pair reaches 2 GiB and LoongArch's PCADDU18I and JIRL 128 GiB, the whole of any module:
   neither machine gets veneers */
int snx_elf_call_relocation(unsigned machine, unsigned type) {
    if (machine == EM_PPC64) return type == SNX_PPC_REL24;
    if (machine == EM_AARCH64) return type == SNX_A64_CALL26 || type == SNX_A64_JUMP26;
    if (machine == EM_RISCV || machine == EM_LOONGARCH) return 0;
    return type == R_X86_64_PLT32;
}

int snx_elf_absolute_relocation(unsigned machine, unsigned type) {
    if (machine == EM_PPC64) return type == SNX_PPC_ADDR64 || type == SNX_PPC_TOC;
    if (machine == EM_RISCV) return type == SNX_RV_64;
    if (machine == EM_LOONGARCH) return type == SNX_LA_64;
    return type == (machine == EM_AARCH64 ? (unsigned)SNX_A64_ABS64 : (unsigned)R_X86_64_64);
}

unsigned snx_elf_relative_type(unsigned machine) {
    return machine == EM_AARCH64     ? (unsigned)SNX_A64_RELATIVE
           : machine == EM_PPC64     ? (unsigned)SNX_PPC_RELATIVE
           : machine == EM_RISCV     ? (unsigned)SNX_RV_RELATIVE
           : machine == EM_LOONGARCH ? (unsigned)SNX_LA_RELATIVE
                                     : (unsigned)R_X86_64_RELATIVE;
}

int snx_elf_riscv_lo12(unsigned machine, unsigned type) {
    return machine == EM_RISCV && (type == SNX_RV_PCREL_LO12_I || type == SNX_RV_PCREL_LO12_S);
}

long snx_elf_riscv_hi20(const Elf64_Rela* rela, size_t n, uint64_t offset) {
    for (size_t k = 0; k < n; ++k) {
        unsigned type = ELF64_R_TYPE(rela[k].r_info);
        if (rela[k].r_offset == offset && (type == SNX_RV_PCREL_HI20 || type == SNX_RV_GOT_HI20)) return (long)k;
    }
    return -1;
}

/* the section at the end of its kind, which grows by it */
static void place(SnxElfObject* o, size_t j, uint64_t* size, uint64_t* kind_align) {
    uint64_t align = o->sh[j].sh_addralign ? o->sh[j].sh_addralign : 1;
    *size = snx_align_up(*size, align);
    o->position[j] = *size;
    *size += o->sh[j].sh_size;
    if (align > *kind_align) *kind_align = align;
}

void snx_elf_layout(SnxElfObject* objs, size_t count, const SnxElfPlace* text, uint64_t* kind_size,
                    uint64_t* kind_align) {
    size_t text_count = 0;
    for (size_t k = 0; k < SNX_ELF_KINDS; ++k) {
        kind_size[k] = 0;
        kind_align[k] = 1;
    }
    for (size_t i = 0; text && i < count; ++i)
        for (size_t j = 0; j < objs[i].eh->e_shnum; ++j) text_count += objs[i].kind[j] == SNX_ELF_TEXT;
    for (size_t i = 0; i < text_count; ++i)
        place(&objs[text[i].object], text[i].section, &kind_size[SNX_ELF_TEXT], &kind_align[SNX_ELF_TEXT]);
    for (size_t k = 0; k < SNX_ELF_KINDS; ++k)
        for (size_t i = 0; i < count && !(text && k == SNX_ELF_TEXT); ++i) {
            SnxElfObject* o = &objs[i];
            for (size_t j = 0; j < o->eh->e_shnum; ++j)
                if (o->kind[j] == (int)k) place(o, j, &kind_size[k], &kind_align[k]);
            if (k == SNX_ELF_TEXT && o->veneers) { /* right after the object's code, in reach of all of it */
                o->island = (kind_size[k] + 3) & ~(uint64_t)3;
                kind_size[k] = o->island + (o->eh->e_machine == EM_PPC64 ? SNX_ELF_PPC_VENEER_SIZE : SNX_ELF_VENEER_SIZE) *
                                               (uint64_t)o->veneers;
            }
        }
}

uint64_t snx_elf_text_size(const SnxElfObject* objs, size_t count) {
    uint64_t size = 0;
    for (size_t i = 0; i < count; ++i)
        for (size_t j = 0; j < objs[i].eh->e_shnum; ++j)
            if (objs[i].kind[j] == SNX_ELF_TEXT) size += objs[i].sh[j].sh_size + objs[i].sh[j].sh_addralign;
    return size;
}

/* ---- AArch64 instruction fields. Everything in uint64_t: a negative displacement is its two's complement, which
   the masks cut to the field as the instruction wants it, with no signed shift or overflow on the way ---- */

/* whether value, read as signed, fits a signed field of bits: [-2^(bits-1), 2^(bits-1)) moved up by 2^(bits-1) is
   [0, 2^bits) */
static int fits(uint64_t value, unsigned bits) { return (value + ((uint64_t)1 << (bits - 1))) >> bits == 0; }
/* ADRP's displacement from the page of P to the page of target, in pages: its 21 bits reach 4 GiB either way */
static int adrp(unsigned char* p, uint64_t target, uint64_t P) {
    uint64_t pages = snx_link_page_of(target) - snx_link_page_of(P);
    if (!fits(pages, 33)) return SNX_LINK_RANGE;
    snx_link_adr_field(p, pages >> 12);
    return 0;
}
/* a PC-relative branch's displacement, in instructions, into its field */
static int branch(unsigned char* p, uint64_t displacement, unsigned bits, unsigned shift) {
    if (!fits(displacement, bits + 2)) return SNX_LINK_RANGE;
    if (displacement & 3) return SNX_LINK_MISALIGNED;
    snx_link_field(p, displacement >> 2, shift, bits);
    return 0;
}

/* the bytes a relocation writes (4 for the types snx_elf_relocate refuses) */
static size_t field_size(unsigned machine, unsigned type) {
    if (type == 0) return 0; /* R_X86_64_NONE, R_AARCH64_NONE, R_PPC64_NONE */
    if (machine == EM_PPC64) switch (type) {
            case SNX_PPC_ENTRY:
            case SNX_PPC_PLTSEQ:
            case SNX_PPC_PLTCALL: return 0;
            case SNX_PPC_ADDR64:
            case SNX_PPC_REL64:
            case SNX_PPC_TOC: return 8;
            case SNX_PPC_REL16:
            case SNX_PPC_REL16_LO:
            case SNX_PPC_REL16_HI:
            case SNX_PPC_REL16_HA:
            case SNX_PPC_ADDR16_LO:
            case SNX_PPC_ADDR16_HI:
            case SNX_PPC_ADDR16_HA:
            case SNX_PPC_ADDR16_LO_DS:
            case SNX_PPC_TOC16:
            case SNX_PPC_TOC16_LO:
            case SNX_PPC_TOC16_HI:
            case SNX_PPC_TOC16_HA:
            case SNX_PPC_TOC16_DS:
            case SNX_PPC_TOC16_LO_DS: return 2;
            default: return 4;
        }
    if (machine == EM_AARCH64) return type == SNX_A64_ABS64 || type == SNX_A64_PREL64 ? 8 : 4;
    if (machine == EM_RISCV) switch (type) {
            case SNX_RV_RELAX:
            case SNX_RV_ALIGN: return 0;
            case SNX_RV_64:
            case SNX_RV_ADD64:
            case SNX_RV_SUB64:
            case SNX_RV_CALL:
            case SNX_RV_CALL_PLT: return 8; /* AUIPC and JALR */
            case SNX_RV_ADD16:
            case SNX_RV_SUB16:
            case SNX_RV_SET16:
            case SNX_RV_RVC_BRANCH:
            case SNX_RV_RVC_JUMP: return 2;
            case SNX_RV_ADD8:
            case SNX_RV_SUB8:
            case SNX_RV_SET8:
            case SNX_RV_SET6:
            case SNX_RV_SUB6: return 1;
            default: return 4;
        }
    if (machine == EM_LOONGARCH) switch (type) {
            case SNX_LA_MARK_LA:
            case SNX_LA_MARK_PCREL:
            case SNX_LA_RELAX:
            case SNX_LA_ALIGN: return 0;
            case SNX_LA_64:
            case SNX_LA_64_PCREL:
            case SNX_LA_ADD64:
            case SNX_LA_SUB64:
            case SNX_LA_CALL36: return 8; /* PCADDU18I and JIRL */
            case SNX_LA_ADD24:
            case SNX_LA_SUB24: return 3;
            case SNX_LA_ADD16:
            case SNX_LA_SUB16: return 2;
            case SNX_LA_ADD8:
            case SNX_LA_SUB8:
            case SNX_LA_ADD6:
            case SNX_LA_SUB6: return 1;
            default: return 4;
        }
    switch (type) {
        case R_X86_64_64:
        case R_X86_64_PC64:
        case R_X86_64_GOTOFF64:
        case R_X86_64_GOTPC64: return 8;
        default: return 4;
    }
}

static int relocate_x86(unsigned char* p, unsigned type, uint64_t S, int64_t A, uint64_t P, uint64_t got,
                        uint64_t slot) {
    switch (type) {
        case R_X86_64_NONE: return 0;
        case R_X86_64_64: snx_st_le64(p, S + (uint64_t)A); return 0;
        case R_X86_64_PC32:
        case R_X86_64_PLT32: return snx_link_rel32(p, S + (uint64_t)A, P);
        case R_X86_64_PC64: snx_st_le64(p, S + (uint64_t)A - P); return 0;
        case R_X86_64_GOTPCREL:
        case R_X86_64_GOTPCRELX:
        case R_X86_64_REX_GOTPCRELX: return snx_link_rel32(p, slot + (uint64_t)A, P);
        case R_X86_64_GOTOFF64: snx_st_le64(p, S + (uint64_t)A - got); return 0;
        case R_X86_64_GOTPC32: return snx_link_rel32(p, got + (uint64_t)A, P);
        case R_X86_64_GOTPC64: snx_st_le64(p, got + (uint64_t)A - P); return 0;
        default: return SNX_LINK_UNSUPPORTED;
    }
}

/* AArch64's, as ELF for the Arm 64-bit Architecture (5.7) computes them: S + A, its page, or its distance from P */
static int relocate_a64(unsigned char* p, unsigned type, uint64_t S, int64_t A, uint64_t P, uint64_t slot) {
    uint64_t target = S + (uint64_t)A;
    unsigned scale;
    switch (type) {
        case SNX_A64_NONE: return 0;
        case SNX_A64_ABS64: snx_st_le64(p, target); return 0;
        case SNX_A64_PREL64: snx_st_le64(p, target - P); return 0;
        case SNX_A64_PREL32: return snx_link_rel32(p, target, P);
        case SNX_A64_ADR_PREL_LO21:
            if (!fits(target - P, 21)) return SNX_LINK_RANGE;
            snx_link_adr_field(p, target - P);
            return 0;
        case SNX_A64_ADR_PREL_PG_HI21: return adrp(p, target, P);
        case SNX_A64_ADR_PREL_PG_HI21_NC:
            snx_link_adr_field(p, (snx_link_page_of(target) - snx_link_page_of(P)) >> 12);
            return 0;
        case SNX_A64_ADD_ABS_LO12_NC: snx_link_field(p, target & 0xFFF, 10, 12); return 0;
        case SNX_A64_LDST8_ABS_LO12_NC:
        case SNX_A64_LDST16_ABS_LO12_NC:
        case SNX_A64_LDST32_ABS_LO12_NC:
        case SNX_A64_LDST64_ABS_LO12_NC:
        case SNX_A64_LDST128_ABS_LO12_NC:
            /* the offset is scaled by the access's size, which the address must be a multiple of */
            scale = type == SNX_A64_LDST8_ABS_LO12_NC    ? 0
                    : type == SNX_A64_LDST16_ABS_LO12_NC ? 1
                    : type == SNX_A64_LDST32_ABS_LO12_NC ? 2
                    : type == SNX_A64_LDST64_ABS_LO12_NC ? 3
                                                         : 4;
            if (target & (((uint64_t)1 << scale) - 1)) return SNX_LINK_MISALIGNED;
            snx_link_field(p, (target & 0xFFF) >> scale, 10, 12);
            return 0;
        case SNX_A64_ADR_GOT_PAGE: return adrp(p, slot + (uint64_t)A, P);
        case SNX_A64_LD64_GOT_LO12_NC: /* LDR Xt: the slot's offset in its page, in 8-byte units */
            if (slot & 7) return SNX_LINK_MISALIGNED;
            snx_link_field(p, (slot & 0xFFF) >> 3, 10, 12);
            return 0;
        case SNX_A64_CALL26:
        case SNX_A64_JUMP26: return branch(p, target - P, 26, 0);
        case SNX_A64_CONDBR19: return branch(p, target - P, 19, 5);
        case SNX_A64_TSTBR14: return branch(p, target - P, 14, 5);
        default: return SNX_LINK_UNSUPPORTED;
    }
}

/* ---- PowerPC64, in the host's byte order as its objects are: a half16 relocation's offset is its halfword's own,
   in either byte order ---- */

static void host16(unsigned char* p, uint16_t v) { memcpy(p, &v, 2); }
static void host32(unsigned char* p, uint32_t v) { memcpy(p, &v, 4); }
static void host64(unsigned char* p, uint64_t v) { memcpy(p, &v, 8); }
static uint16_t host_get16(const unsigned char* p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
static uint32_t host_get32(const unsigned char* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint16_t ppc_lo(uint64_t v) { return (uint16_t)v; }
static uint16_t ppc_hi(uint64_t v) { return (uint16_t)(v >> 16); }
static uint16_t ppc_ha(uint64_t v) { return (uint16_t)((v + 0x8000) >> 16); }
/* whether value, read as signed, fits 32 bits: the reach of an @ha, @l pair */
static int fits32(uint64_t value) { return fits(value, 32); }
/* a DS-form field: the low two bits are the instruction's own */
static int ds_field(unsigned char* p, uint64_t value) {
    if (value & 3) return SNX_LINK_MISALIGNED;
    host16(p, (uint16_t)((host_get16(p) & 3) | (ppc_lo(value) & 0xFFFC)));
    return 0;
}
/* a branch's displacement into the instruction's mask */
static int ppc_branch(unsigned char* p, uint64_t displacement, unsigned bits, uint32_t mask) {
    if (!fits(displacement, bits)) return SNX_LINK_RANGE;
    host32(p, (host_get32(p) & ~mask) | ((uint32_t)displacement & mask));
    return 0;
}

/* PowerPC64's, as the 64-bit ELF V2 ABI (and the V1 supplement) compute them: S + A, its distance from P or from the
   TOC, and their halves */
static int relocate_ppc(unsigned char* p, unsigned type, uint64_t S, int64_t A, uint64_t P, uint64_t toc) {
    uint64_t value = S + (uint64_t)A, relative = value - P, toc_relative = value - toc;
    switch (type) {
        case SNX_PPC_NONE:
        case SNX_PPC_ENTRY: /* the linker may rewrite a global entry's TOC setup; it needn't */
        case SNX_PPC_PLTSEQ:
        case SNX_PPC_PLTCALL: /* markers of an inline PLT call sequence, which code without a PLT leaves as it is */
            return 0;
        case SNX_PPC_TOC: host64(p, toc); return 0; /* .TOC., in an ELFv1 descriptor */
        case SNX_PPC_ADDR64: host64(p, value); return 0;
        case SNX_PPC_REL64: host64(p, relative); return 0;
        case SNX_PPC_REL32:
            if (!fits32(relative)) return SNX_LINK_RANGE;
            host32(p, (uint32_t)relative);
            return 0;
        case SNX_PPC_REL24: return ppc_branch(p, relative, 26, 0x03FFFFFCu);
        case SNX_PPC_REL14: return ppc_branch(p, relative, 16, 0xFFFCu);
        case SNX_PPC_REL16:
            if (!fits(relative, 16)) return SNX_LINK_RANGE;
            host16(p, ppc_lo(relative));
            return 0;
        case SNX_PPC_REL16_LO: host16(p, ppc_lo(relative)); return 0;
        case SNX_PPC_REL16_HI: host16(p, ppc_hi(relative)); return 0;
        case SNX_PPC_REL16_HA:
            if (!fits32(relative)) return SNX_LINK_RANGE;
            host16(p, ppc_ha(relative));
            return 0;
        case SNX_PPC_ADDR16_LO: host16(p, ppc_lo(value)); return 0;
        case SNX_PPC_ADDR16_HI: host16(p, ppc_hi(value)); return 0;
        case SNX_PPC_ADDR16_HA: host16(p, ppc_ha(value)); return 0;
        case SNX_PPC_ADDR16_LO_DS: return ds_field(p, value);
        case SNX_PPC_TOC16:
            if (!fits(toc_relative, 16)) return SNX_LINK_RANGE;
            host16(p, ppc_lo(toc_relative));
            return 0;
        case SNX_PPC_TOC16_LO: host16(p, ppc_lo(toc_relative)); return 0;
        case SNX_PPC_TOC16_HI: host16(p, ppc_hi(toc_relative)); return 0;
        case SNX_PPC_TOC16_HA:
            if (!fits32(toc_relative)) return SNX_LINK_RANGE;
            host16(p, ppc_ha(toc_relative));
            return 0;
        case SNX_PPC_TOC16_DS:
            if (!fits(toc_relative, 16)) return SNX_LINK_RANGE;
            return ds_field(p, toc_relative);
        case SNX_PPC_TOC16_LO_DS: return ds_field(p, toc_relative);
        default: return SNX_LINK_UNSUPPORTED;
    }
}

/* ---- the label-difference relocations RISC-V and LoongArch share: a little-endian field of 1 to 8 bytes, added to,
   taken from or replaced, as the psABIs compute word8 to word64 and the 6-bit fields of DWARF's advance ops ---- */

static uint64_t get_le(const unsigned char* p, size_t bytes) {
    uint64_t v = 0;
    for (size_t i = 0; i < bytes; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static void put_le(unsigned char* p, size_t bytes, uint64_t v) {
    for (size_t i = 0; i < bytes; ++i) p[i] = (unsigned char)(v >> (8 * i));
}
/* op 1 adds value, -1 takes it away, 0 writes it */
static void word(unsigned char* p, size_t bytes, int op, uint64_t value) {
    uint64_t old = get_le(p, bytes);
    put_le(p, bytes, op > 0 ? old + value : op < 0 ? old - value : value);
}
/* the low 6 bits of a byte, the two above them kept */
static void word6(unsigned char* p, int op, uint64_t value) {
    uint64_t old = p[0] & 0x3F;
    uint64_t v = op > 0 ? old + value : op < 0 ? old - value : value;
    p[0] = (unsigned char)((p[0] & 0xC0) | (v & 0x3F));
}

/* ---- RISC-V, little-endian: an immediate's bits scatter across its instruction by format ---- */

/* the high 20 bits of a displacement an AUIPC (U-type) holds, rounded for the low 12 that its pair sign-extends */
static uint64_t rv_hi20(uint64_t displacement) { return (displacement + 0x800) >> 12; }
/* whether an AUIPC pair reaches displacement: 32 bits less the 0x800 the rounding takes */
static int rv_pair_fits(uint64_t displacement) { return fits(displacement + 0x800, 32); }
static void rv_u(unsigned char* p, uint64_t hi20) { snx_link_field(p, hi20, 12, 20); }
static void rv_i(unsigned char* p, uint64_t lo12) { snx_link_field(p, lo12, 20, 12); }
static void rv_s(unsigned char* p, uint64_t lo12) {
    snx_link_field(p, lo12 >> 5, 25, 7);
    snx_link_field(p, lo12, 7, 5);
}
/* B-type, BEQ and the like: imm[12|10:5] at 25, imm[4:1|11] at 7, 4 KiB either way */
static int rv_b(unsigned char* p, uint64_t d) {
    if (!fits(d, 13)) return SNX_LINK_RANGE;
    if (d & 1) return SNX_LINK_MISALIGNED;
    snx_link_field(p, (d >> 12 & 1) << 6 | (d >> 5 & 0x3F), 25, 7);
    snx_link_field(p, (d >> 1 & 0xF) << 1 | (d >> 11 & 1), 7, 5);
    return 0;
}
/* J-type, JAL: imm[20|10:1|11|19:12] at 12, 1 MiB either way */
static int rv_j(unsigned char* p, uint64_t d) {
    if (!fits(d, 21)) return SNX_LINK_RANGE;
    if (d & 1) return SNX_LINK_MISALIGNED;
    snx_link_field(p, (d >> 20 & 1) << 19 | (d >> 1 & 0x3FF) << 9 | (d >> 11 & 1) << 8 | (d >> 12 & 0xFF), 12, 20);
    return 0;
}
/* CB-type, C.BEQZ and C.BNEZ: offset[8|4:3] at 10, [7:6|2:1|5] at 2, 256 bytes either way */
static int rv_cb(unsigned char* p, uint64_t d) {
    uint16_t insn = snx_ld_le16(p) & 0xE383u;
    if (!fits(d, 9)) return SNX_LINK_RANGE;
    if (d & 1) return SNX_LINK_MISALIGNED;
    insn |=
        (uint16_t)((d >> 8 & 1) << 12 | (d >> 3 & 3) << 10 | (d >> 6 & 3) << 5 | (d >> 1 & 3) << 3 | (d >> 5 & 1) << 2);
    snx_st_le16(p, insn);
    return 0;
}
/* CJ-type, C.J: offset[11|4|9:8|10|6|7|3:1|5] at 2, 2 KiB either way */
static int rv_cj(unsigned char* p, uint64_t d) {
    uint16_t insn = snx_ld_le16(p) & 0xE003u;
    if (!fits(d, 12)) return SNX_LINK_RANGE;
    if (d & 1) return SNX_LINK_MISALIGNED;
    insn |= (uint16_t)((d >> 11 & 1) << 12 | (d >> 4 & 1) << 11 | (d >> 8 & 3) << 9 | (d >> 10 & 1) << 8 |
                       (d >> 6 & 1) << 7 | (d >> 7 & 1) << 6 | (d >> 1 & 7) << 3 | (d >> 5 & 1) << 2);
    snx_st_le16(p, insn);
    return 0;
}

/* RISC-V's, as its psABI computes them: S + A, its distance from P, a GOT slot's, and their high and low halves */
static int relocate_rv(unsigned char* p, unsigned type, uint64_t S, int64_t A, uint64_t P, uint64_t slot) {
    uint64_t target = S + (uint64_t)A, d = target - P;
    switch (type) {
        case SNX_RV_NONE:
        case SNX_RV_RELAX:
            return 0; /* leaving code unrelaxed is always correct */
        /* ALIGN wants padding taken out, which only relaxation puts in: an object made with it is refused */
        case SNX_RV_ALIGN: return SNX_LINK_UNSUPPORTED;
        case SNX_RV_32:
            if (target >> 32 && !fits(target, 32)) return SNX_LINK_RANGE;
            put_le(p, 4, target);
            return 0;
        case SNX_RV_64: put_le(p, 8, target); return 0;
        case SNX_RV_32_PCREL:
        case SNX_RV_PLT32:
            if (!fits(d, 32)) return SNX_LINK_RANGE;
            put_le(p, 4, d);
            return 0;
        case SNX_RV_BRANCH: return rv_b(p, d);
        case SNX_RV_JAL: return rv_j(p, d);
        case SNX_RV_RVC_BRANCH: return rv_cb(p, d);
        case SNX_RV_RVC_JUMP: return rv_cj(p, d);
        case SNX_RV_CALL:
        case SNX_RV_CALL_PLT: /* AUIPC then JALR */
            if (!rv_pair_fits(d)) return SNX_LINK_RANGE;
            rv_u(p, rv_hi20(d));
            rv_i(p + 4, d & 0xFFF);
            return 0;
        case SNX_RV_PCREL_HI20:
            if (!rv_pair_fits(d)) return SNX_LINK_RANGE;
            rv_u(p, rv_hi20(d));
            return 0;
        case SNX_RV_GOT_HI20:
            d = slot + (uint64_t)A - P;
            if (!rv_pair_fits(d)) return SNX_LINK_RANGE;
            rv_u(p, rv_hi20(d));
            return 0;
        case SNX_RV_PCREL_LO12_I: /* S is its HI20's displacement */
            if (A) return SNX_LINK_UNSUPPORTED;
            rv_i(p, S & 0xFFF);
            return 0;
        case SNX_RV_PCREL_LO12_S:
            if (A) return SNX_LINK_UNSUPPORTED;
            rv_s(p, S & 0xFFF);
            return 0;
        case SNX_RV_ADD8: word(p, 1, 1, target); return 0;
        case SNX_RV_ADD16: word(p, 2, 1, target); return 0;
        case SNX_RV_ADD32: word(p, 4, 1, target); return 0;
        case SNX_RV_ADD64: word(p, 8, 1, target); return 0;
        case SNX_RV_SUB8: word(p, 1, -1, target); return 0;
        case SNX_RV_SUB16: word(p, 2, -1, target); return 0;
        case SNX_RV_SUB32: word(p, 4, -1, target); return 0;
        case SNX_RV_SUB64: word(p, 8, -1, target); return 0;
        case SNX_RV_SET8: word(p, 1, 0, target); return 0;
        case SNX_RV_SET16: word(p, 2, 0, target); return 0;
        case SNX_RV_SET32: word(p, 4, 0, target); return 0;
        case SNX_RV_SET6: word6(p, 0, target); return 0;
        case SNX_RV_SUB6: word6(p, -1, target); return 0;
        default: return SNX_LINK_UNSUPPORTED;
    }
}

/* ---- LoongArch, little-endian: si20 at 5, si12 at 10, offs16 at 10 and the wider branch offsets split in two ---- */

/* the pages from P's to that of target, rounded for the low 12 bits ADDI.D and LD.D sign-extend */
static uint64_t la_pages(uint64_t target, uint64_t P) { return snx_link_page_of(target + 0x800) - snx_link_page_of(P); }
static int la_hi20(unsigned char* p, uint64_t target, uint64_t P) {
    uint64_t pages = la_pages(target, P);
    if (!fits(pages, 32)) return SNX_LINK_RANGE;
    snx_link_field(p, pages >> 12, 5, 20);
    return 0;
}
/* B16 (BEQ and the like), B21 (BEQZ, BNEZ) and B26 (B, BL), in instructions: offs[15:0] at 10, the rest at 0 */
static int la_branch(unsigned char* p, uint64_t d, unsigned bits) {
    if (!fits(d, bits + 2)) return SNX_LINK_RANGE;
    if (d & 3) return SNX_LINK_MISALIGNED;
    snx_link_field(p, d >> 2, 10, 16);
    if (bits > 16) snx_link_field(p, d >> 18, 0, bits - 16);
    return 0;
}

/* LoongArch's, as its psABI computes them */
static int relocate_la(unsigned char* p, unsigned type, uint64_t S, int64_t A, uint64_t P, uint64_t slot) {
    uint64_t target = S + (uint64_t)A, d = target - P, at = slot + (uint64_t)A;
    switch (type) {
        case SNX_LA_NONE:
        case SNX_LA_MARK_LA:
        case SNX_LA_MARK_PCREL:
        case SNX_LA_RELAX: return 0;
        case SNX_LA_ALIGN: return SNX_LINK_UNSUPPORTED; /* as RISC-V's */
        case SNX_LA_32:
            if (target >> 32 && !fits(target, 32)) return SNX_LINK_RANGE;
            put_le(p, 4, target);
            return 0;
        case SNX_LA_64: put_le(p, 8, target); return 0;
        case SNX_LA_32_PCREL:
            if (!fits(d, 32)) return SNX_LINK_RANGE;
            put_le(p, 4, d);
            return 0;
        case SNX_LA_64_PCREL: put_le(p, 8, d); return 0;
        case SNX_LA_B16: return la_branch(p, d, 16);
        case SNX_LA_B21: return la_branch(p, d, 21);
        case SNX_LA_B26: return la_branch(p, d, 26);
        case SNX_LA_PCALA_HI20: return la_hi20(p, target, P);
        case SNX_LA_PCALA_LO12: snx_link_field(p, target & 0xFFF, 10, 12); return 0;
        case SNX_LA_GOT_PC_HI20: return la_hi20(p, at, P);
        case SNX_LA_GOT_PC_LO12: snx_link_field(p, at & 0xFFF, 10, 12); return 0;
        case SNX_LA_PCREL20_S2: /* PCADDI */
            if (!fits(d, 22)) return SNX_LINK_RANGE;
            if (d & 3) return SNX_LINK_MISALIGNED;
            snx_link_field(p, d >> 2, 5, 20);
            return 0;
        case SNX_LA_CALL36: /* PCADDU18I then JIRL: 18 bits of the displacement in JIRL, sign-extended */
            if (!fits(d + 0x20000, 38)) return SNX_LINK_RANGE;
            if (d & 3) return SNX_LINK_MISALIGNED;
            snx_link_field(p, (d + 0x20000) >> 18, 5, 20);
            snx_link_field(p + 4, d >> 2, 10, 16);
            return 0;
        case SNX_LA_ADD8: word(p, 1, 1, target); return 0;
        case SNX_LA_ADD16: word(p, 2, 1, target); return 0;
        case SNX_LA_ADD24: word(p, 3, 1, target); return 0;
        case SNX_LA_ADD32: word(p, 4, 1, target); return 0;
        case SNX_LA_ADD64: word(p, 8, 1, target); return 0;
        case SNX_LA_SUB8: word(p, 1, -1, target); return 0;
        case SNX_LA_SUB16: word(p, 2, -1, target); return 0;
        case SNX_LA_SUB24: word(p, 3, -1, target); return 0;
        case SNX_LA_SUB32: word(p, 4, -1, target); return 0;
        case SNX_LA_SUB64: word(p, 8, -1, target); return 0;
        case SNX_LA_ADD6: word6(p, 1, target); return 0;
        case SNX_LA_SUB6: word6(p, -1, target); return 0;
        default: return SNX_LINK_UNSUPPORTED;
    }
}

int snx_elf_relocate(unsigned machine, unsigned char* p, size_t room, unsigned type, uint64_t S, int64_t A, uint64_t P,
                     uint64_t got, uint64_t slot) {
    if (field_size(machine, type) > room) return SNX_LINK_OUTSIDE;
    if (machine == EM_AARCH64) return relocate_a64(p, type, S, A, P, slot);
    if (machine == EM_PPC64) return relocate_ppc(p, type, S, A, P, got);
    if (machine == EM_RISCV) return relocate_rv(p, type, S, A, P, slot);
    if (machine == EM_LOONGARCH) return relocate_la(p, type, S, A, P, slot);
    return relocate_x86(p, type, S, A, P, got, slot);
}

int snx_elf_veneer(unsigned char* v, uint64_t at, uint64_t target, int indirect) {
    if (indirect && (target & 7)) return SNX_LINK_MISALIGNED;
    snx_st_le32(v, 0x90000010u); /* ADRP x16, target */
    int rc = adrp(v, target, at);
    if (rc) return rc;
    if (indirect) {
        snx_st_le32(v + 4, 0xF9400210u); /* LDR x16, [x16, :lo12:target], scaled by 8 */
        snx_link_field(v + 4, (target & 0xFFF) >> 3, 10, 12);
    } else {
        snx_st_le32(v + 4, 0x91000210u); /* ADD x16, x16, :lo12:target */
        snx_link_field(v + 4, target & 0xFFF, 10, 12);
    }
    snx_st_le32(v + 8, 0xD61F0200u); /* BR x16 */
    return 0;
}

int snx_elf_branch_veneer(unsigned char* p, uint64_t P, unsigned char* v, uint64_t at, uint64_t target, int indirect) {
    int rc = snx_elf_veneer(v, at, target, indirect);
    if (rc) return rc;
    return snx_elf_relocate(EM_AARCH64, p, 4, SNX_A64_CALL26, at, 0, P, 0, 0);
}

/* ELFv2's local entry offset: (1 << n) >> 2 instructions for st_other's n, none for 0 and 1 */
static uint64_t ppc_local_entry(unsigned char other) { return (uint64_t)((1u << ((other >> 5) & 7)) >> 2) << 2; }

int snx_elf_ppc_entry(const SnxElfObject* objs, const SnxLinkNames* globals, unsigned abi, size_t def_object,
                      size_t def_index, uint64_t S, int64_t A, uint64_t* global, uint64_t* local, char* error,
                      size_t error_size) {
    const SnxElfObject* o = &objs[def_object];
    const Elf64_Sym* sym = &o->syms[def_index];
    size_t code_object, code_index;
    int absolute;
    *global = *local = S + (uint64_t)A;
    if (abi == SNX_ELF_PPC_ELFV2) {
        if (((sym->st_other >> 5) & 7) == 1)
            return snx_link_fail(error, error_size, "%s: a call to %s, which doesn't keep r2 (PC-relative code)",
                                 o->input->name,
                        snx_elf_symbol_name(o, sym));
        *local += ppc_local_entry(sym->st_other);
        return 0;
    }
    if (sym->st_shndx >= o->eh->e_shnum || strcmp(snx_elf_section_name(o, sym->st_shndx), ".opd")) return 0;
    for (size_t j = 0; j < o->eh->e_shnum; ++j) {
        const Elf64_Rela* r = (const Elf64_Rela*)(o->input->data + o->sh[j].sh_offset);
        size_t n = o->sh[j].sh_size / sizeof(Elf64_Rela);
        if (o->sh[j].sh_type != SHT_RELA || o->sh[j].sh_info != sym->st_shndx) continue;
        for (size_t k = 0; k < n; ++k)
            if (r[k].r_offset == sym->st_value + (uint64_t)A && ELF64_R_TYPE(r[k].r_info) == SNX_PPC_ADDR64) {
                int where = snx_elf_definition(objs, globals, def_object, ELF64_R_SYM(r[k].r_info), &code_object,
                                               &code_index, error, error_size);
                if (where < 0) return -1;
                if (where != SNX_ELF_HERE) break;
                *global = *local = snx_elf_symbol_address(objs, code_object, code_index, &absolute) +
                                   (uint64_t)r[k].r_addend;
                return 0;
            }
    }
    return snx_link_fail(error, error_size, "%s: .opd descriptor of %s without its code", o->input->name,
                         snx_elf_symbol_name(o, sym));
}

int snx_elf_ppc_branch_veneer(unsigned char* p, uint64_t P, unsigned char* v, uint64_t at, uint64_t target,
                              uint64_t toc) {
    uint64_t offset = target - toc;
    if (!fits32(offset)) return SNX_LINK_RANGE;
    host32(v, 0x3D820000u | ppc_ha(offset));     /* addis r12, r2, offset@ha */
    host32(v + 4, 0x398C0000u | ppc_lo(offset)); /* addi r12, r12, offset@l */
    host32(v + 8, 0x7D8903A6u);                  /* mtctr r12 */
    host32(v + 12, 0x4E800420u);                 /* bctr */
    return snx_elf_relocate(EM_PPC64, p, 4, SNX_PPC_REL24, at, 0, P, toc, 0);
}
#else
typedef int snx_elf_object_unused; /* ISO C wants a declaration in every file */
#endif
