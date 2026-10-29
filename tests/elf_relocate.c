/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The ELF relocations the AOT linker (link_elf.c) and the JIT loader (code_loader.c) share (elf_object.c), checked on
   any host: every AArch64 relocation type and veneer against encodings worked out by hand from the Arm ARM, PowerPC64's
   instruction fields and veneer from the ELF V2 ABI, then
   AArch64 objects from the LLVM backend (when its LLVM has the AArch64 target) linked into a module and loaded into a
   code arena whose code is only read, never run: calls out of reach must land on stubs that reach their targets (in a
   load larger than B's reach, on the veneers after each object's code), and GOT slots must hold their symbols'
   addresses. An arena sized by snx_code_need must hold what it counted. */
#include "common/utilities/common.h"
#include "common/cpu/recompiler/llvm/elf_object.h"
#include "common/cpu/recompiler/llvm/code_loader_int.h" /* the regions' use, against snx_code_need's */
#include "common/cpu/recompiler/llvm/ir.h"
#include <llvm-c/Target.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static uint32_t get32(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const unsigned char* p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

static void check(int ok, const char* what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        ++failures;
    }
}

/* the instruction insn relocated: whether snx_elf_relocate gave rc and left want */
static void a64(const char* what, uint32_t insn, unsigned type, uint64_t S, int64_t A, uint64_t P, uint64_t slot,
                int rc, uint32_t want) {
    unsigned char p[4];
    int got;
    snx_link_put32(p, insn);
    got = snx_elf_relocate(EM_AARCH64, p, sizeof(p), type, S, A, P, 0, slot);
    if (got != rc || get32(p) != want) {
        printf("FAIL: %s: rc %d (want %d), %08x (want %08x)\n", what, got, rc, (unsigned)get32(p), (unsigned)want);
        ++failures;
    }
}

static void data_relocations(void) {
    unsigned char p[8];
    memset(p, 0xAA, sizeof(p));
    check(!snx_elf_relocate(EM_AARCH64, p, 8, SNX_A64_ABS64, 0x1122334455667788ull, 8, 0, 0, 0) &&
              get64(p) == 0x1122334455667790ull,
          "ABS64");
    check(!snx_elf_relocate(EM_AARCH64, p, 8, SNX_A64_PREL64, 0x20000, 0, 0x10000, 0, 0) && get64(p) == 0x10000,
          "PREL64");
    check(!snx_elf_relocate(EM_AARCH64, p, 8, SNX_A64_PREL64, 0x10000, -8, 0x20000, 0, 0) &&
              get64(p) == (uint64_t)-0x10008,
          "PREL64 backwards");
    memset(p, 0xAA, sizeof(p));
    check(!snx_elf_relocate(EM_AARCH64, p, 4, SNX_A64_PREL32, 0x10000, 0, 0x20000, 0, 0) && get32(p) == 0xFFFF0000u &&
              p[4] == 0xAA,
          "PREL32 backwards, 4 bytes");
    check(snx_elf_relocate(EM_AARCH64, p, 4, SNX_A64_PREL32, 0x80000000ull + 0x1000, 0, 0x1000, 0, 0) == SNX_LINK_RANGE,
          "PREL32 beyond 2 GiB");
    check(snx_elf_relocate(EM_AARCH64, p, 7, SNX_A64_ABS64, 0, 0, 0, 0, 0) == SNX_LINK_OUTSIDE, "ABS64 in 7 bytes");
    check(snx_elf_relocate(EM_AARCH64, p, 3, SNX_A64_CALL26, 0, 0, 0, 0, 0) == SNX_LINK_OUTSIDE, "CALL26 in 3 bytes");
    check(!snx_elf_relocate(EM_AARCH64, p, 0, SNX_A64_NONE, 0, 0, 0, 0, 0), "NONE in no room");
    /* the machine decides what a number means: 1 is R_X86_64_64 but an ILP32 relocation of AArch64's */
    check(snx_elf_relocate(EM_AARCH64, p, 8, 1, 0, 0, 0, 0, 0) == SNX_LINK_UNSUPPORTED, "AArch64 type 1");
    check(snx_elf_relocate(EM_X86_64, p, 8, SNX_A64_ABS64, 0, 0, 0, 0, 0) == SNX_LINK_UNSUPPORTED, "x86-64 type 257");
    /* x86-64's, as before */
    check(!snx_elf_relocate(EM_X86_64, p, 4, R_X86_64_PC32, 0x2000, -4, 0x1000, 0, 0) && get32(p) == 0xFFC,
          "x86-64 PC32");
    check(!snx_elf_relocate(EM_X86_64, p, 4, R_X86_64_REX_GOTPCRELX, 0, -4, 0x1000, 0, 0x3000) && get32(p) == 0x1FFC,
          "x86-64 REX_GOTPCRELX");
    check(snx_elf_relocate(EM_X86_64, p, 4, R_X86_64_PLT32, 0x100001000ull, -4, 0x1000, 0, 0) == SNX_LINK_RANGE,
          "x86-64 PLT32 beyond 2 GiB");
    check(snx_elf_relocate(EM_X86_64, p, 4, R_X86_64_GOTPC64, 0, 0, 0, 0, 0) == SNX_LINK_OUTSIDE,
          "x86-64 GOTPC64 in 4 bytes");
}

static void instruction_relocations(void) {
    const uint64_t P = 0x400123 & ~(uint64_t)3;
    /* ADR x0: immlo at 29, immhi at 5; 0x12345 is immlo 1, immhi 0x48D1 */
    a64("ADR", 0x10000000u, SNX_A64_ADR_PREL_LO21, P + 0x12345, 0, P, 0, 0, 0x30091A20u);
    a64("ADR backwards", 0x10000000u, SNX_A64_ADR_PREL_LO21, P, -4, P, 0, 0, 0x10FFFFE0u);
    a64("ADR beyond 1 MiB", 0x10000000u, SNX_A64_ADR_PREL_LO21, P + 0x100000, 0, P, 0, SNX_LINK_RANGE, 0x10000000u);
    /* ADRP x1: 0x12345000 - 0x400000 is 0x11F45 pages, immlo 1, immhi 0x47D1 */
    a64("ADRP", 0x90000001u, SNX_A64_ADR_PREL_PG_HI21, 0x12345678, 0, P, 0, 0, 0xB008FA21u);
    a64("ADRP addend", 0x90000001u, SNX_A64_ADR_PREL_PG_HI21, 0x12345000, 0x678, P, 0, 0, 0xB008FA21u);
    a64("ADRP NC", 0x90000001u, SNX_A64_ADR_PREL_PG_HI21_NC, 0x12345678, 0, P, 0, 0, 0xB008FA21u);
    /* one page back: -1 pages, all 21 bits set */
    a64("ADRP backwards", 0x90000001u, SNX_A64_ADR_PREL_PG_HI21, P - 0x1000, 0, P, 0, 0, 0xF0FFFFE1u);
    a64("ADRP beyond 4 GiB", 0x90000001u, SNX_A64_ADR_PREL_PG_HI21, P + ((uint64_t)4 << 30), 0, P, 0, SNX_LINK_RANGE,
        0x90000001u);
    /* NC keeps the low 21 bits of the page count: 8 GiB and a page is 0x200001 pages, written as 1 */
    a64("ADRP NC beyond 4 GiB wraps", 0x90000001u, SNX_A64_ADR_PREL_PG_HI21_NC, P + ((uint64_t)8 << 30) + 0x1000, 0,
        P & ~(uint64_t)0xFFF, 0, 0, 0xB0000001u);
    /* ADD x1, x1, #imm12 at 10 */
    a64("ADD lo12", 0x91000021u, SNX_A64_ADD_ABS_LO12_NC, 0x12345678, 0, P, 0, 0, 0x9119E021u);
    /* loads: the low 12 bits scaled by the access size */
    a64("LDRB lo12", 0x39400022u, SNX_A64_LDST8_ABS_LO12_NC, 0x12345679, 0, P, 0, 0, 0x3959E422u);
    a64("LDRH lo12", 0x79400022u, SNX_A64_LDST16_ABS_LO12_NC, 0x1234567A, 0, P, 0, 0, 0x794CF422u);
    a64("LDRH misaligned", 0x79400022u, SNX_A64_LDST16_ABS_LO12_NC, 0x12345679, 0, P, 0, SNX_LINK_MISALIGNED,
        0x79400022u);
    a64("LDR w lo12", 0xB9400022u, SNX_A64_LDST32_ABS_LO12_NC, 0x12345678, 0, P, 0, 0, 0xB9467822u);
    a64("LDR x lo12", 0xF9400022u, SNX_A64_LDST64_ABS_LO12_NC, 0x12345678, 0, P, 0, 0, 0xF9433C22u);
    a64("LDR q lo12", 0x3DC00022u, SNX_A64_LDST128_ABS_LO12_NC, 0x12345670, 0, P, 0, 0, 0x3DC19C22u);
    a64("LDR q misaligned", 0x3DC00022u, SNX_A64_LDST128_ABS_LO12_NC, 0x12345678, 0, P, 0, SNX_LINK_MISALIGNED,
        0x3DC00022u);
    /* GOT: the slot's page (0x7000 - 0x1000: 6 pages, immlo 2, immhi 1) and its offset in 8-byte units */
    a64("ADRP got", 0x90000003u, SNX_A64_ADR_GOT_PAGE, 0, 0, 0x1000, 0x7008, 0, 0xD0000023u);
    a64("LDR got", 0xF9400063u, SNX_A64_LD64_GOT_LO12_NC, 0, 0, 0x1000, 0x7008, 0, 0xF9400463u);
    a64("LDR got misaligned", 0xF9400063u, SNX_A64_LD64_GOT_LO12_NC, 0, 0, 0x1000, 0x7004, SNX_LINK_MISALIGNED,
        0xF9400063u);
    /* branches: imm26 at 0, imm19 and imm14 at 5, in instructions */
    a64("BL", 0x94000000u, SNX_A64_CALL26, P + 0x1000, 0, P, 0, 0, 0x94000400u);
    a64("BL backwards", 0x94000000u, SNX_A64_CALL26, P - 8, 0, P, 0, 0, 0x97FFFFFEu);
    a64("BL at -128 MiB", 0x94000000u, SNX_A64_CALL26, P - ((uint64_t)128 << 20), 0, P, 0, 0, 0x96000000u);
    a64("BL at +128 MiB", 0x94000000u, SNX_A64_CALL26, P + ((uint64_t)128 << 20), 0, P, 0, SNX_LINK_RANGE, 0x94000000u);
    a64("BL misaligned", 0x94000000u, SNX_A64_CALL26, P + 2, 0, P, 0, SNX_LINK_MISALIGNED, 0x94000000u);
    a64("B", 0x14000000u, SNX_A64_JUMP26, P + 4, 0, P, 0, 0, 0x14000001u);
    a64("B.EQ", 0x54000000u, SNX_A64_CONDBR19, P + 0x40, 0, P, 0, 0, 0x54000200u);
    a64("B.EQ backwards", 0x54000000u, SNX_A64_CONDBR19, P - 4, 0, P, 0, 0, 0x54FFFFE0u);
    a64("B.EQ beyond 1 MiB", 0x54000000u, SNX_A64_CONDBR19, P + 0x100000, 0, P, 0, SNX_LINK_RANGE, 0x54000000u);
    a64("CBZ", 0xB4000000u, SNX_A64_CONDBR19, P + 8, 0, P, 0, 0, 0xB4000040u);
    a64("TBZ", 0x36180000u, SNX_A64_TSTBR14, P + 0x20, 0, P, 0, 0, 0x36180100u);
    a64("TBZ at -32 KiB", 0x36180000u, SNX_A64_TSTBR14, P - 0x8000, 0, P, 0, 0, 0x361C0000u);
    a64("TBZ at +32 KiB", 0x36180000u, SNX_A64_TSTBR14, P + 0x8000, 0, P, 0, SNX_LINK_RANGE, 0x36180000u);
}

static void veneers(void) {
    unsigned char v[SNX_ELF_VENEER_SIZE], p[4];
    /* ADRP x16 (0x2345 pages: immlo 1, immhi 0x8D1), ADD x16, x16, #0x678, BR x16 */
    check(!snx_elf_veneer(v, 0x10000000, 0x12345678, 0) && get32(v) == 0xB0011A30u && get32(v + 4) == 0x9119E210u &&
              get32(v + 8) == 0xD61F0200u,
          "direct veneer");
    /* ADRP x16 (3 pages: immlo 3), LDR x16, [x16, #8], BR x16 */
    check(!snx_elf_veneer(v, 0x10000000, 0x10003008, 1) && get32(v) == 0xF0000010u && get32(v + 4) == 0xF9400610u &&
              get32(v + 8) == 0xD61F0200u,
          "indirect veneer");
    check(snx_elf_veneer(v, 0x10000000, 0x10003004, 1) == SNX_LINK_MISALIGNED, "indirect veneer, misaligned slot");
    check(snx_elf_veneer(v, 0x10000000, 0x10000000 + ((uint64_t)5 << 30), 0) == SNX_LINK_RANGE, "veneer beyond 4 GiB");
    snx_link_put32(p, 0x94000000u);
    check(!snx_elf_branch_veneer(p, 0x1000, v, 0x2000, 0x40001234, 0) && get32(p) == 0x94000400u &&
              get32(v + 4) == 0x9108D210u,
          "branch through a veneer");
    snx_link_put32(p, 0x94000000u);
    check(snx_elf_branch_veneer(p, 0x1000, v, 0x1000 + ((uint64_t)200 << 20), 0x40001234, 0) == SNX_LINK_RANGE &&
              get32(p) == 0x94000000u,
          "branch to a veneer out of reach");
}

/* PowerPC64's fields are in the host's byte order, as its objects are */
static uint32_t host32(const unsigned char* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint16_t host16(const unsigned char* p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
static void set32(unsigned char* p, uint32_t v) { memcpy(p, &v, 4); }
static void set16(unsigned char* p, uint16_t v) { memcpy(p, &v, 2); }

static void ppc_relocations(void) {
    unsigned char p[8], v[SNX_ELF_PPC_VENEER_SIZE];
    const uint64_t toc = 0x18000;
    set32(p, 0x48000001u); /* bl */
    check(!snx_elf_relocate(EM_PPC64, p, 4, SNX_PPC_REL24, 0x2000, 0, 0x1000, toc, 0) && host32(p) == 0x48001001u,
          "PowerPC64 REL24");
    set32(p, 0x48000001u);
    check(snx_elf_relocate(EM_PPC64, p, 4, SNX_PPC_REL24, 0x1000 + ((uint64_t)32 << 20), 0, 0x1000, toc, 0) ==
                  SNX_LINK_RANGE &&
              host32(p) == 0x48000001u,
          "PowerPC64 REL24 at +32 MiB");
    /* 0x38010 is 0x20010 past the TOC: @ha 2, @l 0x0010; a DS field keeps the instruction's two low bits */
    set16(p, 0);
    check(!snx_elf_relocate(EM_PPC64, p, 2, SNX_PPC_TOC16_HA, 0x38000, 0x10, 0, toc, 0) && host16(p) == 2,
          "PowerPC64 TOC16_HA");
    set16(p, 0x0001);
    check(!snx_elf_relocate(EM_PPC64, p, 2, SNX_PPC_TOC16_LO_DS, 0x38010, 0, 0, toc, 0) && host16(p) == 0x0011,
          "PowerPC64 TOC16_LO_DS");
    check(snx_elf_relocate(EM_PPC64, p, 2, SNX_PPC_TOC16_LO_DS, 0x38012, 0, 0, toc, 0) == SNX_LINK_MISALIGNED,
          "PowerPC64 TOC16_LO_DS, misaligned");
    /* @ha rounds: 0x28000 past the TOC + 0x8000 is 0x30000 */
    check(!snx_elf_relocate(EM_PPC64, p, 2, SNX_PPC_TOC16_HA, 0x40000, 0, 0, toc, 0) && host16(p) == 3,
          "PowerPC64 TOC16_HA rounding");
    /* backwards, -0x1F000: @ha 0xFFFE, @l 0x1000 */
    check(!snx_elf_relocate(EM_PPC64, p, 2, SNX_PPC_REL16_HA, 0x1000, 0, 0x20000, toc, 0) && host16(p) == 0xFFFE,
          "PowerPC64 REL16_HA backwards");
    check(!snx_elf_relocate(EM_PPC64, p, 2, SNX_PPC_REL16_LO, 0x1000, 0, 0x20000, toc, 0) && host16(p) == 0x1000,
          "PowerPC64 REL16_LO backwards");
    check(!snx_elf_relocate(EM_PPC64, p, 8, SNX_PPC_TOC, 0, 0, 0, toc, 0) && !memcmp(p, &toc, 8), "PowerPC64 TOC");
    check(snx_elf_relocate(EM_PPC64, p, 1, SNX_PPC_TOC16_LO, 0, 0, 0, toc, 0) == SNX_LINK_OUTSIDE,
          "PowerPC64 TOC16_LO in 1 byte");
    check(snx_elf_relocate(EM_PPC64, p, 8, SNX_PPC_ADDR32, 0, 0, 0, toc, 0) == SNX_LINK_UNSUPPORTED,
          "PowerPC64 ADDR32");
    check(!snx_elf_got_relocation(EM_PPC64, R_X86_64_GOTPCREL) && snx_elf_call_relocation(EM_PPC64, SNX_PPC_REL24) &&
              snx_elf_absolute_relocation(EM_PPC64, SNX_PPC_TOC) && !snx_elf_absolute_relocation(EM_PPC64, R_X86_64_64),
          "PowerPC64 relocation classes");
    /* 0x123456 is 0x10B456 past the TOC: addis r12, r2, 0x11; addi r12, r12, -0x4BAA; mtctr r12; bctr; and the bl to
       the veneer at +0x2000 */
    set32(p, 0x48000001u);
    check(!snx_elf_ppc_branch_veneer(p, 0x1000, v, 0x3000, 0x123456, toc) && host32(v) == 0x3D820011u &&
              host32(v + 4) == 0x398CB456u && host32(v + 8) == 0x7D8903A6u && host32(v + 12) == 0x4E800420u &&
              host32(p) == 0x48002001u,
          "PowerPC64 branch through a veneer");
    check(snx_elf_ppc_branch_veneer(p, 0x1000, v, 0x3000, toc + ((uint64_t)3 << 30), toc) == SNX_LINK_RANGE,
          "PowerPC64 veneer beyond the TOC's 2 GiB");
}

/* RISC-V's and LoongArch's instruction relocated: whether snx_elf_relocate gave rc and left want */
static void rv_la(unsigned machine, const char* what, uint32_t insn, unsigned type, uint64_t S, int64_t A, uint64_t P,
                  uint64_t slot, int rc, uint32_t want) {
    unsigned char p[4];
    int got;
    snx_link_put32(p, insn);
    got = snx_elf_relocate(machine, p, sizeof(p), type, S, A, P, 0, slot);
    if (got != rc || get32(p) != want) {
        printf("FAIL: %s %s: rc %d (want %d), %08x (want %08x)\n", machine == EM_RISCV ? "RISC-V" : "LoongArch", what,
               got, rc, (unsigned)get32(p), (unsigned)want);
        ++failures;
    }
}
static uint16_t get16(const unsigned char* p) { return (uint16_t)(p[0] | p[1] << 8); }
static void put16(unsigned char* p, uint16_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

/* RISC-V's fields, worked out from the ISA manual's U, I, S, B, J, CB and CJ formats */
static void riscv_relocations(void) {
    const uint64_t P = 0x1000;
    unsigned char p[8];
    Elf64_Rela rela[3];
    /* AUIPC ra, JALR ra: 0x12344678 away is 0x12344 pages and 0x678 */
    snx_link_put32(p, 0x00000097u);
    snx_link_put32(p + 4, 0x000080E7u);
    check(!snx_elf_relocate(EM_RISCV, p, 8, SNX_RV_CALL_PLT, 0x12345678, 0, P, 0, 0) && get32(p) == 0x12344097u &&
              get32(p + 4) == 0x678080E7u,
          "RISC-V CALL_PLT");
    /* 0x1800 away: the low half 0x800 is -0x800 sign-extended, so the high half rounds up to 2 */
    snx_link_put32(p, 0x00000097u);
    snx_link_put32(p + 4, 0x000080E7u);
    check(!snx_elf_relocate(EM_RISCV, p, 8, SNX_RV_CALL_PLT, P + 0x1800, 0, P, 0, 0) && get32(p) == 0x00002097u &&
              get32(p + 4) == 0x800080E7u,
          "RISC-V CALL_PLT, its low half negative");
    check(snx_elf_relocate(EM_RISCV, p, 8, SNX_RV_CALL_PLT, P + 0x80000000ull, 0, P, 0, 0) == SNX_LINK_RANGE,
          "RISC-V CALL_PLT beyond 2 GiB");
    check(snx_elf_relocate(EM_RISCV, p, 7, SNX_RV_CALL_PLT, P, 0, P, 0, 0) == SNX_LINK_OUTSIDE,
          "RISC-V CALL_PLT in 7 bytes");
    /* AUIPC a0: a GOT slot 0x6008 away, a symbol 0x12344678 away; the ADDI and SD halves take that displacement as S */
    rv_la(EM_RISCV, "GOT_HI20", 0x00000517u, SNX_RV_GOT_HI20, 0, 0, P, P + 0x6008, 0, 0x00006517u);
    rv_la(EM_RISCV, "PCREL_HI20", 0x00000517u, SNX_RV_PCREL_HI20, 0x12345678, 0, P, 0, 0, 0x12344517u);
    rv_la(EM_RISCV, "PCREL_LO12_I", 0x00050513u, SNX_RV_PCREL_LO12_I, 0x12344678, 0, P + 4, 0, 0, 0x67850513u);
    rv_la(EM_RISCV, "PCREL_LO12_I with an addend", 0x00050513u, SNX_RV_PCREL_LO12_I, 0x12344678, 4, P + 4, 0,
          SNX_LINK_UNSUPPORTED, 0x00050513u);
    rv_la(EM_RISCV, "PCREL_LO12_S", 0x00B53023u, SNX_RV_PCREL_LO12_S, 0x12344678, 0, P + 4, 0, 0, 0x66B53C23u);
    /* BEQ a0, a1 (4 KiB either way) and JAL ra (1 MiB) */
    rv_la(EM_RISCV, "BRANCH", 0x00B50063u, SNX_RV_BRANCH, P + 16, 0, P, 0, 0, 0x00B50863u);
    rv_la(EM_RISCV, "BRANCH at -4 KiB", 0x00B50063u, SNX_RV_BRANCH, P - 0x1000, 0, P, 0, 0, 0x80B50063u);
    rv_la(EM_RISCV, "BRANCH at +4 KiB", 0x00B50063u, SNX_RV_BRANCH, P + 0x1000, 0, P, 0, SNX_LINK_RANGE, 0x00B50063u);
    rv_la(EM_RISCV, "JAL", 0x000000EFu, SNX_RV_JAL, P + 8, 0, P, 0, 0, 0x008000EFu);
    rv_la(EM_RISCV, "JAL 2 KiB on", 0x000000EFu, SNX_RV_JAL, P + 0x800, 0, P, 0, 0, 0x001000EFu);
    rv_la(EM_RISCV, "JAL misaligned", 0x000000EFu, SNX_RV_JAL, P + 1, 0, P, 0, SNX_LINK_MISALIGNED, 0x000000EFu);
    /* C.J and C.BEQZ a0, two bytes each */
    put16(p, 0xA001u);
    check(!snx_elf_relocate(EM_RISCV, p, 2, SNX_RV_RVC_JUMP, P + 4, 0, P, 0, 0) && get16(p) == 0xA011u,
          "RISC-V RVC_JUMP");
    put16(p, 0xC101u);
    check(!snx_elf_relocate(EM_RISCV, p, 2, SNX_RV_RVC_BRANCH, P + 4, 0, P, 0, 0) && get16(p) == 0xC111u,
          "RISC-V RVC_BRANCH");
    /* label differences, and DWARF's 6-bit advance fields */
    snx_link_put32(p, 0x100);
    check(!snx_elf_relocate(EM_RISCV, p, 4, SNX_RV_ADD32, 0x50, 0, 0, 0, 0) && get32(p) == 0x150 &&
              !snx_elf_relocate(EM_RISCV, p, 4, SNX_RV_SUB32, 0x20, 0, 0, 0, 0) && get32(p) == 0x130,
          "RISC-V ADD32 and SUB32");
    p[0] = 0xC5;
    check(!snx_elf_relocate(EM_RISCV, p, 1, SNX_RV_SUB6, 2, 0, 0, 0, 0) && p[0] == 0xC3 &&
              !snx_elf_relocate(EM_RISCV, p, 1, SNX_RV_SET6, 0x3F, 0, 0, 0, 0) && p[0] == 0xFF,
          "RISC-V SUB6 and SET6 keep the byte's top two bits");
    check(!snx_elf_relocate(EM_RISCV, p, 4, SNX_RV_32_PCREL, P - 0x10, 0, P, 0, 0) && get32(p) == 0xFFFFFFF0u,
          "RISC-V 32_PCREL backwards");
    check(!snx_elf_relocate(EM_RISCV, p, 0, SNX_RV_RELAX, 0, 0, 0, 0, 0) &&
              snx_elf_relocate(EM_RISCV, p, 4, SNX_RV_ALIGN, 0, 0, 0, 0, 0) == SNX_LINK_UNSUPPORTED,
          "RISC-V RELAX left alone, ALIGN refused");
    check(snx_elf_got_relocation(EM_RISCV, SNX_RV_GOT_HI20) && !snx_elf_got_relocation(EM_RISCV, SNX_RV_PCREL_LO12_I) &&
              snx_elf_absolute_relocation(EM_RISCV, SNX_RV_64) && !snx_elf_call_relocation(EM_RISCV, SNX_RV_CALL_PLT) &&
              snx_elf_relative_type(EM_RISCV) == SNX_RV_RELATIVE && snx_elf_riscv_lo12(EM_RISCV, SNX_RV_PCREL_LO12_S) &&
              !snx_elf_riscv_lo12(EM_X86_64, SNX_RV_PCREL_LO12_I),
          "RISC-V relocation classes");
    /* a low half's HI20, found by its label's offset */
    memset(rela, 0, sizeof(rela));
    rela[0].r_offset = 0x10;
    rela[0].r_info = ELF64_R_INFO(1ull, SNX_RV_PCREL_HI20);
    rela[1].r_offset = 0x14;
    rela[1].r_info = ELF64_R_INFO(2ull, SNX_RV_PCREL_LO12_I);
    rela[2].r_offset = 0x20;
    rela[2].r_info = ELF64_R_INFO(1ull, SNX_RV_GOT_HI20);
    check(snx_elf_riscv_hi20(rela, 3, 0x10) == 0 && snx_elf_riscv_hi20(rela, 3, 0x20) == 2 &&
              snx_elf_riscv_hi20(rela, 3, 0x14) == -1,
          "RISC-V HI20 found by its offset");
}

/* LoongArch's fields, from its ISA manual's 1RI20, 2RI12, 2RI16 and branch formats */
static void loongarch_relocations(void) {
    const uint64_t P = 0x1000;
    unsigned char p[8];
    /* PCALAU12I a0 and ADDI.D a0, a0: 0x12345678 is 0x12344 pages past P's and 0x678 */
    rv_la(EM_LOONGARCH, "PCALA_HI20", 0x1A000004u, SNX_LA_PCALA_HI20, 0x12345678, 0, P, 0, 0, 0x1A246884u);
    rv_la(EM_LOONGARCH, "PCALA_LO12", 0x02C00084u, SNX_LA_PCALA_LO12, 0x12345678, 0, P, 0, 0, 0x02D9E084u);
    /* 0xA00 in the page is -0x600 sign-extended: the page rounds up */
    rv_la(EM_LOONGARCH, "PCALA_HI20 rounded", 0x1A000004u, SNX_LA_PCALA_HI20, 0x12345A00, 0, P, 0, 0, 0x1A2468A4u);
    rv_la(EM_LOONGARCH, "PCALA_LO12 rounded", 0x02C00084u, SNX_LA_PCALA_LO12, 0x12345A00, 0, P, 0, 0, 0x02E80084u);
    /* PCALAU12I a0 and LD.D a0, a0 of a GOT slot 0x6008 away */
    rv_la(EM_LOONGARCH, "GOT_PC_HI20", 0x1A000004u, SNX_LA_GOT_PC_HI20, 0, 0, P, 0x7008, 0, 0x1A0000C4u);
    rv_la(EM_LOONGARCH, "GOT_PC_LO12", 0x28C00084u, SNX_LA_GOT_PC_LO12, 0, 0, P, 0x7008, 0, 0x28C02084u);
    /* PCADDU18I ra, JIRL ra, ra: 0x12344678 is 0x48D << 18 and 0x119E instructions */
    snx_link_put32(p, 0x1E000001u);
    snx_link_put32(p + 4, 0x4C000021u);
    check(!snx_elf_relocate(EM_LOONGARCH, p, 8, SNX_LA_CALL36, 0x12345678, 0, P, 0, 0) && get32(p) == 0x1E0091A1u &&
              get32(p + 4) == 0x4C467821u,
          "LoongArch CALL36");
    /* 0x30000 sets bit 17: JIRL's 0xC000 is -0x4000 instructions, so PCADDU18I rounds up to 1 */
    snx_link_put32(p, 0x1E000001u);
    snx_link_put32(p + 4, 0x4C000021u);
    check(!snx_elf_relocate(EM_LOONGARCH, p, 8, SNX_LA_CALL36, P + 0x30000, 0, P, 0, 0) && get32(p) == 0x1E000021u &&
              get32(p + 4) == 0x4F000021u,
          "LoongArch CALL36, JIRL's half negative");
    check(snx_elf_relocate(EM_LOONGARCH, p, 8, SNX_LA_CALL36, P + ((uint64_t)128 << 30), 0, P, 0, 0) == SNX_LINK_RANGE,
          "LoongArch CALL36 beyond 128 GiB");
    /* B (26 bits), BEQZ a0 (21) and BEQ a0, a1 (16), in instructions */
    rv_la(EM_LOONGARCH, "B26", 0x50000000u, SNX_LA_B26, P + 0x100, 0, P, 0, 0, 0x50010000u);
    rv_la(EM_LOONGARCH, "B26 backwards", 0x50000000u, SNX_LA_B26, P - 4, 0, P, 0, 0, 0x53FFFFFFu);
    rv_la(EM_LOONGARCH, "B26 misaligned", 0x50000000u, SNX_LA_B26, P + 2, 0, P, 0, SNX_LINK_MISALIGNED, 0x50000000u);
    rv_la(EM_LOONGARCH, "B21", 0x40000080u, SNX_LA_B21, P + 8, 0, P, 0, 0, 0x40000880u);
    rv_la(EM_LOONGARCH, "B16", 0x58000085u, SNX_LA_B16, P + 8, 0, P, 0, 0, 0x58000885u);
    rv_la(EM_LOONGARCH, "B16 at +128 KiB", 0x58000085u, SNX_LA_B16, P + 0x20000, 0, P, 0, SNX_LINK_RANGE, 0x58000085u);
    /* label differences and data */
    snx_link_put32(p, 0x100);
    check(!snx_elf_relocate(EM_LOONGARCH, p, 4, SNX_LA_ADD32, 0x50, 0, 0, 0, 0) && get32(p) == 0x150 &&
              !snx_elf_relocate(EM_LOONGARCH, p, 4, SNX_LA_SUB32, 0x20, 0, 0, 0, 0) && get32(p) == 0x130,
          "LoongArch ADD32 and SUB32");
    check(!snx_elf_relocate(EM_LOONGARCH, p, 4, SNX_LA_32_PCREL, P - 0x10, 0, P, 0, 0) && get32(p) == 0xFFFFFFF0u,
          "LoongArch 32_PCREL backwards");
    check(!snx_elf_relocate(EM_LOONGARCH, p, 8, SNX_LA_64, 0x1122334455667788ull, 8, 0, 0, 0) &&
              get64(p) == 0x1122334455667790ull,
          "LoongArch 64");
    check(!snx_elf_relocate(EM_LOONGARCH, p, 0, SNX_LA_MARK_LA, 0, 0, 0, 0, 0) &&
              snx_elf_relocate(EM_LOONGARCH, p, 4, SNX_LA_ALIGN, 0, 0, 0, 0, 0) == SNX_LINK_UNSUPPORTED,
          "LoongArch markers left alone, ALIGN refused");
    check(snx_elf_got_relocation(EM_LOONGARCH, SNX_LA_GOT_PC_LO12) &&
              snx_elf_absolute_relocation(EM_LOONGARCH, SNX_LA_64) &&
              !snx_elf_call_relocation(EM_LOONGARCH, SNX_LA_CALL36) &&
              snx_elf_relative_type(EM_LOONGARCH) == SNX_LA_RELATIVE,
          "LoongArch relocation classes");
}

/* ---- the LLVM backend's AArch64 objects ---- */

static int64_t sign(uint64_t value, unsigned bits) {
    uint64_t m = (uint64_t)1 << (bits - 1);
    return (int64_t)((value ^ m) - m);
}
/* where an ADRP at address points: the page of its target */
static uint64_t adrp_page(uint32_t insn, uint64_t address) {
    uint64_t imm = (insn >> 29 & 3) | (uint64_t)(insn >> 5 & 0x7FFFF) << 2;
    return (address & ~(uint64_t)0xFFF) + (uint64_t)(sign(imm, 21) * 4096);
}
/* where a B or BL at address goes */
static uint64_t branch_target(uint32_t insn, uint64_t address) {
    return address + (uint64_t)(sign(insn & 0x3FFFFFF, 26) * 4);
}
static int is_branch(uint32_t insn) { return (insn & 0x7C000000u) == 0x14000000u; } /* B, BL */

/* where the stub at address jumps: through its GOT slot when *indirect */
static uint64_t stub_target(uint64_t address, int* indirect) {
    const unsigned char* s = (const unsigned char*)(size_t)address;
    uint32_t adrp = get32(s), second = get32(s + 4);
    uint64_t page = adrp_page(adrp, address);
    if ((adrp & 0x9F00001Fu) != 0x90000010u || get32(s + 8) != 0xD61F0200u) return 0;
    *indirect = (second & 0xFFC003FFu) == 0xF9400210u;
    if (*indirect) return page + (second >> 10 & 0xFFF) * 8;
    if ((second & 0xFFC003FFu) != 0x91000210u) return 0;
    return page + (second >> 10 & 0xFFF);
}

/* An AArch64 object: caller() returns callee(*data + 1) + 1; callee is hidden (BL), data default visibility (the GOT) */
static int object_for(const char* triple, const char* callee_name, const char* data_name, const char* caller_name,
                      unsigned char** data, size_t* size) {
    char error[1024];
    SnxIr* ir = snx_ir_new_target("caller", '2', triple, error, sizeof(error));
    LLVMValueRef value, callee, caller, args[1], result;
    LLVMTypeRef params[1];
    if (!ir) return snx_error("%s", error);
    params[0] = ir->t_i64;
    value = LLVMAddGlobal(ir->mod, ir->t_i64, data_name);
    callee = ir_extern(ir, callee_name, ir->t_i64, params, 1);
    caller = LLVMAddFunction(ir->mod, caller_name, LLVMFunctionType(ir->t_i64, NULL, 0, 0));
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, caller, "entry"));
    args[0] = LLVMBuildAdd(ir->b, LLVMBuildLoad2(ir->b, ir->t_i64, value, "v"), LLVMConstInt(ir->t_i64, 1, 0), "a");
    result = LLVMBuildCall2(ir->b, LLVMGlobalGetValueType(callee), callee, args, 1, "c");
    LLVMBuildRet(ir->b, LLVMBuildAdd(ir->b, result, LLVMConstInt(ir->t_i64, 1, 0), "r"));
    if (snx_ir_object(ir, '2', data, size, error, sizeof(error))) return snx_error("%s", error);
    snx_ir_free(ir);
    return 0;
}

#ifndef __APPLE__ /* the AOT checks' objects: macOS links no ELF module (link_elf.c) */
static int object(const char* callee_name, const char* data_name, const char* caller_name, unsigned char** data,
                  size_t* size) {
    return object_for("aarch64-unknown-linux-gnu", callee_name, data_name, caller_name, data, size);
}
#endif

/* the definitions object() calls into */
static int definitions_for(const char* triple, unsigned char** data, size_t* size) {
    char error[1024];
    SnxIr* ir = snx_ir_new_target("definitions", '2', triple, error, sizeof(error));
    LLVMValueRef value, callee;
    LLVMTypeRef params[1];
    if (!ir) return snx_error("%s", error);
    params[0] = ir->t_i64;
    value = LLVMAddGlobal(ir->mod, ir->t_i64, "ext_data");
    LLVMSetInitializer(value, LLVMConstInt(ir->t_i64, 41, 0));
    callee = ir_extern(ir, "ext_fn", ir->t_i64, params, 1);
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, callee, "entry"));
    LLVMBuildRet(ir->b, LLVMBuildMul(ir->b, LLVMGetParam(callee, 0), LLVMConstInt(ir->t_i64, 3, 0), "m"));
    if (snx_ir_object(ir, '2', data, size, error, sizeof(error))) return snx_error("%s", error);
    snx_ir_free(ir);
    return 0;
}
#ifndef __APPLE__
static int definitions(unsigned char** data, size_t* size) {
    return definitions_for("aarch64-unknown-linux-gnu", data, size);
}
#endif

/* the first B or BL of the function at address, before its RET: its target (0 for none) */
static uint64_t first_branch(uint64_t address) {
    const unsigned char* code = (const unsigned char*)(size_t)address;
    size_t i;
    for (i = 0; i < 64 && get32(code + 4 * i) != 0xD65F03C0u; ++i)
        if (is_branch(get32(code + 4 * i))) return branch_target(get32(code + 4 * i), address + 4 * i);
    return 0;
}
/* the slot the function's first ADRP and LDR pair reads */
static uint64_t first_got_load(uint64_t address) {
    const unsigned char* code = (const unsigned char*)(size_t)address;
    size_t i, j;
    for (i = 0; i < 64; ++i) {
        uint32_t adrp = get32(code + 4 * i);
        if ((adrp & 0x9F000000u) != 0x90000000u) continue;
        for (j = i + 1; j < i + 8 && j < 64; ++j) {
            uint32_t ldr = get32(code + 4 * j);
            if ((ldr & 0xFFC00000u) == 0xF9400000u && (ldr >> 5 & 31) == (adrp & 31))
                return adrp_page(adrp, address + 4 * i) + (ldr >> 10 & 0xFFF) * 8;
        }
    }
    return 0;
}

static SnxU64 probe_data = 0x1234;
static SnxU64 probe_fn(SnxU64 a) { return a; }

/* JIT: triple's objects loaded into an AArch64 arena. ext_fn and ext_data are in this executable, outside the arena:
   the call goes through an indirect stub, the data through a GOT slot */
static int jit_objects(const char* triple) {
    char error[1024];
    unsigned char *caller_data, *second_data;
    size_t caller_size, second_size;
    SnxLinkInput input;
    const char* names[1];
    SnxU64 caller, second, target, slot;
    SnxCodeLoader* loader;
    int indirect = -1;
    if (object_for(triple, "ext_fn", "ext_data", "caller", &caller_data, &caller_size) ||
        object_for(triple, "caller", "ext_data", "second", &second_data, &second_size))
        return 1;
    loader = snx_code_loader_new_machine(0, EM_AARCH64, error, sizeof(error));
    if (!loader) return snx_error("loader: %s", error);
    if (snx_code_define(loader, "ext_fn", (SnxU64)(size_t)&probe_fn) ||
        snx_code_define(loader, "ext_data", (SnxU64)(size_t)&probe_data))
        return snx_error("loader: defined twice");
    input.data = caller_data;
    input.size = caller_size;
    input.name = "caller";
    names[0] = "caller";
    if (snx_code_load(loader, &input, 1, names, 1, &caller, error, sizeof(error)))
        return snx_error("loader (%s): %s", triple, error);
    target = first_branch(caller);
    if (target == (SnxU64)(size_t)&probe_fn)
        printf("the arena is within 128 MiB of this executable: no far call to check\n");
    else {
        check(target && stub_target(target, &indirect) && indirect == 1, "far call through an indirect stub");
        slot = stub_target(target, &indirect);
        check(slot && get64((const unsigned char*)(size_t)slot) == (SnxU64)(size_t)&probe_fn, "stub's GOT slot");
    }
    slot = first_got_load(caller);
    check(slot && get64((const unsigned char*)(size_t)slot) == (SnxU64)(size_t)&probe_data, "data's GOT slot");
    /* a later unit calling the first, in the arena and in reach: directly */
    input.data = second_data;
    input.size = second_size;
    input.name = "second";
    names[0] = "second";
    if (snx_code_load(loader, &input, 1, names, 1, &second, error, sizeof(error)))
        return snx_error("loader (%s): %s", triple, error);
    check(first_branch(second) == caller, "near call to an earlier load, direct");
    /* and one whose callee is in the arena but beyond B's 128 MiB: through a direct stub (ADRP, ADD) */
    if (snx_code_define(loader, "far_in_arena", caller + ((SnxU64)300 << 20))) return snx_error("loader: defined");
    free(second_data);
    if (object_for(triple, "far_in_arena", "ext_data", "third", &second_data, &second_size)) return 1;
    input.data = second_data;
    input.size = second_size;
    input.name = "third";
    names[0] = "third";
    if (snx_code_load(loader, &input, 1, names, 1, &second, error, sizeof(error)))
        return snx_error("loader (%s): %s", triple, error);
    target = first_branch(second);
    check(target && stub_target(target, &indirect) == caller + ((SnxU64)300 << 20) && indirect == 0,
          "call to a far symbol in the arena through a direct stub");
    snx_code_loader_free(loader);
    free(caller_data);
    free(second_data);
    return 0;
}

/* an object of nothing but bytes of code, which makes a load's code as large as bytes say. Module assembly: LLVM gives
   a global's section the global's flags, so a global in a section named .text would be data */
static int filler_object(const char* triple, size_t bytes, unsigned char** data, size_t* size) {
    char error[1024], text[128];
    SnxIr* ir = snx_ir_new_target("filler", '2', triple, error, sizeof(error));
    if (!ir) return snx_error("%s", error);
    snprintf(text, sizeof text, ".section .text.filler,\"ax\",%%progbits\n.space %zu\n", bytes);
    LLVMSetModuleInlineAsm2(ir->mod, text, strlen(text));
    if (snx_ir_object(ir, '2', data, size, error, sizeof(error))) return snx_error("%s", error);
    snx_ir_free(ir);
    return 0;
}

/* JIT: a game's whole code is one load of more than B and BL reach (128 MiB). A call from its first object to a
   definition in its third goes through a veneer in the island after the caller's code, which the call reaches, and
   the veneer to the definition, which the call does not; a call from its last object to this executable, outside the
   arena, through that object's own veneer and a GOT slot of its own, beside the one of the data it reads */
static int large_load(const char* triple) {
    static const char* const objects[4] = {"caller", "filler", "definitions", "outward"};
    const char* names[4] = {"caller", "ext_fn", "outward", "ext_data"};
    char error[1024];
    unsigned char* data[4];
    size_t size[4], i;
    SnxLinkInput inputs[4];
    SnxU64 address[4], veneer, slot, data_slot;
    SnxCodeLoader* loader;
    size_t need[3];
    char what[160];
    int indirect = -1;
    if (object_for(triple, "ext_fn", "ext_data", "caller", &data[0], &size[0]) ||
        filler_object(triple, (size_t)140 << 20, &data[1], &size[1]) || definitions_for(triple, &data[2], &size[2]) ||
        object_for(triple, "probe_far", "ext_data", "outward", &data[3], &size[3]))
        return 1;
    for (i = 0; i < 4; ++i) {
        inputs[i].data = data[i];
        inputs[i].size = size[i];
        inputs[i].name = objects[i];
    }
    /* the arena snx_code_need sizes, which must hold the islands too */
    if (snx_code_need(inputs, 4, need, error, sizeof(error))) return snx_error("need (%s): %s", triple, error);
    loader = snx_code_loader_new_regions_machine(need, EM_AARCH64, error, sizeof(error));
    if (!loader) return snx_error("loader: %s", error);
    if (snx_code_define(loader, "probe_far", (SnxU64)(size_t)&probe_fn)) return snx_error("loader: defined twice");
    if (snx_code_load(loader, inputs, 4, names, 4, address, error, sizeof(error)))
        return snx_error("large load (%s): %s", triple, error);
    veneer = first_branch(address[0]);
    check(veneer > address[0] && veneer - address[0] < ((SnxU64)128 << 20),
          "a large load's call reaches the veneer after its object's code");
    check(stub_target(veneer, &indirect) == address[1] && indirect == 0, "the veneer jumps to the definition directly");
    check(address[1] - address[0] >= ((SnxU64)128 << 20), "the definition is beyond B's reach");
    veneer = first_branch(address[2]);
    if (veneer == (SnxU64)(size_t)&probe_fn)
        printf("the arena is within 128 MiB of this executable: no far call from a large load to check\n");
    else {
        slot = stub_target(veneer, &indirect);
        check(veneer > address[2] && veneer - address[2] < ((SnxU64)128 << 20) && indirect == 1 && slot &&
                  get64((const unsigned char*)(size_t)slot) == (SnxU64)(size_t)&probe_fn,
              "a large load's far call through its object's veneer and GOT slot");
    }
    data_slot = first_got_load(address[2]);
    check(data_slot && get64((const unsigned char*)(size_t)data_slot) == address[3],
          "a large load's data GOT slot, beside its far call's");
    for (i = 0; i < 3; ++i) {
        snprintf(what, sizeof what,
                 "%s: a large load's region %zu holds what snx_code_need counted (%zu bytes used of %zu)", triple, i,
                 loader->region[i].used, need[i]);
        check(loader->region[i].used <= need[i], what);
    }
    snx_code_loader_free(loader);
    for (i = 0; i < 4; ++i) free(data[i]);
    return 0;
}

/* An object of two functions, the first reading data1 (volatile, through the GOT) and calling call1 with it, the
   second the same with data2 and call2: each function's code is its own, the same size whether the two of a pair are
   one name or two */
static int need_object(const char* triple, const char* call1, const char* call2, const char* data1, const char* data2,
                       SnxLinkInput* input) {
    const char* calls[2] = {call1, call2};
    const char* reads[2] = {data1, data2};
    char error[1024], name[8];
    SnxIr* ir = snx_ir_new_target("need", '2', triple, error, sizeof(error));
    LLVMValueRef callee, datum, fn, v, args[1];
    LLVMTypeRef params[1];
    unsigned char* data;
    size_t size;
    int i;
    if (!ir) return snx_error("%s", error);
    params[0] = ir->t_i64;
    for (i = 0; i < 2; ++i) {
        callee = LLVMGetNamedFunction(ir->mod, calls[i]);
        if (!callee) callee = ir_extern(ir, calls[i], ir->t_i64, params, 1);
        datum = LLVMGetNamedGlobal(ir->mod, reads[i]);
        if (!datum) datum = LLVMAddGlobal(ir->mod, ir->t_i64, reads[i]);
        snprintf(name, sizeof name, "need%d", i);
        fn = LLVMAddFunction(ir->mod, name, LLVMFunctionType(ir->t_i64, params, 1, 0));
        LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, fn, "entry"));
        v = LLVMBuildLoad2(ir->b, ir->t_i64, datum, "v");
        LLVMSetVolatile(v, 1);
        args[0] = LLVMBuildAdd(ir->b, LLVMGetParam(fn, 0), v, "a");
        args[0] = LLVMBuildCall2(ir->b, LLVMGlobalGetValueType(callee), callee, args, 1, "c");
        LLVMBuildRet(ir->b, LLVMBuildAdd(ir->b, args[0], LLVMConstInt(ir->t_i64, 1, 0), "r"));
    }
    if (snx_ir_object(ir, '2', &data, &size, error, sizeof(error))) return snx_error("%s", error);
    snx_ir_free(ir);
    input->data = data;
    input->size = size;
    input->name = "need";
    return 0;
}

/* snx_code_need's counts: a stub (stub bytes on this machine) and a GOT slot for each symbol an object calls, a GOT
   slot for each it reads through the GOT, two references to one symbol counted once */
static int need_counts(const char* triple, uint64_t stub) {
    SnxLinkInput one, calls, reads;
    size_t need_one[3], need_calls[3], need_reads[3];
    char error[1024], what[160];
    if (need_object(triple, "f", "f", "a", "a", &one) || need_object(triple, "f", "g", "a", "a", &calls) ||
        need_object(triple, "f", "f", "a", "b", &reads))
        return 1;
    if (snx_code_need(&one, 1, need_one, error, sizeof(error)) ||
        snx_code_need(&calls, 1, need_calls, error, sizeof(error)) ||
        snx_code_need(&reads, 1, need_reads, error, sizeof(error)))
        return snx_error("need (%s): %s", triple, error);
    snprintf(what, sizeof what,
             "%s: a second callee needs a stub of %llu bytes and a GOT slot (code %zu to %zu, "
             "read-only data %zu to %zu)",
             triple, (unsigned long long)stub, need_one[0], need_calls[0], need_one[1], need_calls[1]);
    check(need_calls[0] == need_one[0] + stub && need_calls[1] == need_one[1] + 8 && need_calls[2] == need_one[2],
          what);
    snprintf(what, sizeof what, "%s: a second datum needs a GOT slot (code %zu to %zu, read-only data %zu to %zu)",
             triple, need_one[0], need_reads[0], need_one[1], need_reads[1]);
    check(need_reads[0] == need_one[0] && need_reads[1] == need_one[1] + 8 && need_reads[2] == need_one[2], what);
    free((void*)one.data);
    free((void*)calls.data);
    free((void*)reads.data);
    return 0;
}

/* An object of data only: other_data (data), a table of constants (read-only data) and 4 KiB of zeros (bss) */
static int held_object(const char* triple, SnxLinkInput* input) {
    char error[1024];
    SnxIr* ir = snx_ir_new_target("held", '2', triple, error, sizeof(error));
    LLVMValueRef value, table[64];
    LLVMTypeRef zeros;
    unsigned char* data;
    size_t size;
    int i;
    if (!ir) return snx_error("%s", error);
    value = LLVMAddGlobal(ir->mod, ir->t_i64, "other_data");
    LLVMSetInitializer(value, LLVMConstInt(ir->t_i64, 7, 0));
    for (i = 0; i < 64; ++i) table[i] = LLVMConstInt(ir->t_i64, (unsigned long long)i * 3 + 1, 0);
    value = LLVMAddGlobal(ir->mod, LLVMArrayType(ir->t_i64, 64), "held_table");
    LLVMSetInitializer(value, LLVMConstArray(ir->t_i64, table, 64));
    LLVMSetGlobalConstant(value, 1);
    zeros = LLVMArrayType(ir->t_i8, 4096);
    value = LLVMAddGlobal(ir->mod, zeros, "held_zeros");
    LLVMSetInitializer(value, LLVMConstNull(zeros));
    if (snx_ir_object(ir, '2', &data, &size, error, sizeof(error))) return snx_error("%s", error);
    snx_ir_free(ir);
    input->data = data;
    input->size = size;
    input->name = "held";
    return 0;
}

/* Two loads into an arena of the regions snx_code_need gives them, as a game's runtime library and pack: each region
   holds them, a call out of the arena and one to the first load through stubs and GOT slots included, and the second
   load's data, read-only data and bss */
static int need_holds(const char* triple) {
    SnxLinkInput first, second[3];
    const char* names[1] = {"caller"};
    size_t need_first[3], need_second[3], need[3], i;
    SnxU64 caller;
    SnxCodeLoader* loader;
    char error[1024], what[160];
    unsigned char* data;
    size_t size;
    if (object_for(triple, "ext_fn", "ext_data", "caller", &data, &size)) return 1;
    first.data = data;
    first.size = size;
    first.name = "caller";
    if (object_for(triple, "caller", "ext_data", "second", &data, &size) ||
        need_object(triple, "caller", "ext_fn", "ext_data", "other_data", &second[1]) ||
        held_object(triple, &second[2]))
        return 1;
    second[0].data = data;
    second[0].size = size;
    second[0].name = "second";
    if (snx_code_need(&first, 1, need_first, error, sizeof(error)) ||
        snx_code_need(second, 3, need_second, error, sizeof(error)))
        return snx_error("need (%s): %s", triple, error);
    for (i = 0; i < 3; ++i) need[i] = need_first[i] + need_second[i];
    loader = snx_code_loader_new_regions_machine(need, EM_AARCH64, error, sizeof(error));
    if (!loader) return snx_error("loader: %s", error);
    if (snx_code_define(loader, "ext_fn", (SnxU64)(size_t)&probe_fn) ||
        snx_code_define(loader, "ext_data", (SnxU64)(size_t)&probe_data))
        return snx_error("loader: defined twice");
    if (snx_code_load(loader, &first, 1, names, 1, &caller, error, sizeof(error)) ||
        snx_code_load(loader, second, 3, NULL, 0, NULL, error, sizeof(error)))
        return snx_error("loads into the arena snx_code_need sized (%s): %s", triple, error);
    for (i = 0; i < 3; ++i) {
        snprintf(what, sizeof what, "%s: region %zu holds what snx_code_need counted (%zu bytes used of %zu)", triple,
                 i, loader->region[i].used, need[i]);
        check(loader->region[i].used <= need[i], what);
    }
    snx_code_loader_free(loader);
    free((void*)first.data);
    for (i = 0; i < 3; ++i) free((void*)second[i].data);
    return 0;
}

/* an arena past 2 GiB refused, with the sizes asked */
static void need_too_large(void) {
    size_t size[3] = {(size_t)3 << 30, (size_t)1 << 20, (size_t)1 << 20};
    char error[1024];
    SnxCodeLoader* loader = snx_code_loader_new_regions_machine(size, EM_AARCH64, error, sizeof(error));
    check(!loader && strstr(error, "3072.0 MiB of code, 1.0 MiB of read-only data and 1.0 MiB of data"),
          "an arena of more than 2 GiB refused, saying the sizes asked");
    snx_code_loader_free(loader);
}

static int llvm_objects(void) {
    char error[1024];
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmPrinters();
    LLVMInitializeAllAsmParsers(); /* large_load's filler is module assembly */
    if (snx_ir_init(error, sizeof(error))) return snx_error("%s", error);
    {
        SnxIr* probe = snx_ir_new_target("probe", '2', "aarch64-unknown-linux-gnu", error, sizeof(error));
        if (!probe) {
            printf("skipped the LLVM objects: %s\n", error);
            return 0;
        }
        snx_ir_free(probe);
    }
#ifndef __APPLE__ /* where the AOT linker writes no ELF module (link_elf.c) */
    {
        unsigned char *caller_data, *defs_data;
        size_t caller_size, defs_size, i;
        SnxLinkInput inputs[2];
        SnxLinkOutput linked;
        const char* exports[1] = {"caller"};
        const Elf64_Ehdr* eh;
        if (object("ext_fn", "ext_data", "caller", &caller_data, &caller_size) || definitions(&defs_data, &defs_size))
            return 1;
        /* AOT: one AArch64 module, rebased with R_AARCH64_RELATIVE (the GOT slot of ext_data at least) */
        inputs[0].data = caller_data;
        inputs[0].size = caller_size;
        inputs[0].name = "caller";
        inputs[1].data = defs_data;
        inputs[1].size = defs_size;
        inputs[1].name = "definitions";
        if (snx_link(inputs, 2, exports, 1, "caller.so", &linked, error, sizeof(error)))
            return snx_error("link: %s", error);
        eh = (const Elf64_Ehdr*)(const void*)linked.data;
        check(eh->e_machine == EM_AARCH64 && eh->e_type == ET_DYN, "AArch64 module header");
        {
            const Elf64_Shdr* sh = (const Elf64_Shdr*)(const void*)(linked.data + eh->e_shoff);
            size_t relatives = 0, others = 0;
            for (i = 0; i < eh->e_shnum; ++i)
                if (sh[i].sh_type == SHT_RELA) {
                    const Elf64_Rela* r = (const Elf64_Rela*)(const void*)(linked.data + sh[i].sh_offset);
                    size_t k;
                    for (k = 0; k < sh[i].sh_size / sizeof(Elf64_Rela); ++k)
                        ELF64_R_TYPE(r[k].r_info) == SNX_A64_RELATIVE ? ++relatives : ++others;
                }
            check(relatives >= 1 && !others, "AArch64 module's dynamic relocations are R_AARCH64_RELATIVE");
        }
        free(linked.data);
        free(caller_data);
        free(defs_data);
    }
#endif
    /* the JIT's objects on Linux and on macOS, where they are ELF with Darwin's calling convention (ir.c): both are
       what elf_object.c reads, with the same relocations, and Darwin's names have no leading underscore in ELF */
    if (jit_objects("aarch64-unknown-linux-gnu") || jit_objects("arm64-apple-macosx11.0.0-elf") ||
        need_counts("aarch64-unknown-linux-gnu", SNX_ELF_VENEER_SIZE) || need_holds("aarch64-unknown-linux-gnu") ||
        large_load("aarch64-unknown-linux-gnu"))
        return 1;
    {
        SnxIr* probe = snx_ir_new_target("probe", '2', "x86_64-unknown-linux-gnu", error, sizeof(error));
        if (!probe) {
            printf("skipped snx_code_need's x86-64 counts: %s\n", error);
            return 0;
        }
        snx_ir_free(probe);
    }
    return need_counts("x86_64-unknown-linux-gnu", 8); /* jmp *slot(%rip) and two int3 */
}

#ifndef __APPLE__ /* AOT modules */
/* an exported symbol's address in a linked module, from its .dynsym; 0 for none */
static uint64_t export_address(const unsigned char* module, const char* name) {
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)(const void*)module;
    const Elf64_Shdr* sh = (const Elf64_Shdr*)(const void*)(module + eh->e_shoff);
    size_t i, k;
    for (i = 0; i < eh->e_shnum; ++i)
        if (sh[i].sh_type == SHT_DYNSYM) {
            const Elf64_Sym* sym = (const Elf64_Sym*)(const void*)(module + sh[i].sh_offset);
            const char* names = (const char*)module + sh[sh[i].sh_link].sh_offset;
            for (k = 1; k < sh[i].sh_size / sizeof(Elf64_Sym); ++k)
                if (!strcmp(names + sym[k].st_name, name)) return sym[k].st_value;
        }
    return 0;
}

/* ppc64le objects (ELFv2) linked into a module, on a little-endian host, whose byte order they must share: the call
   must reach ext_fn's local entry, past the two instructions that compute r2 from r12 when it has them, and the only
   dynamic relocations be R_PPC64_RELATIVE */
static int ppc64le_objects(void) {
    char error[1024];
    const char* triple = "powerpc64le-unknown-linux-gnu";
    const char* exports[2] = {"caller", "ext_fn"};
    unsigned char *caller_data, *defs_data;
    size_t caller_size, defs_size, i, relatives = 0, others = 0;
    SnxLinkInput inputs[2];
    SnxLinkOutput linked;
    const Elf64_Ehdr* eh;
    const Elf64_Shdr* sh;
    uint64_t caller, callee, at, target = 0;
    if (snx_elf_host_data() != ELFDATA2LSB) return 0;
    {
        SnxIr* probe = snx_ir_new_target("probe", '2', triple, error, sizeof(error));
        if (!probe) {
            printf("skipped the PowerPC64 objects: %s\n", error);
            return 0;
        }
        snx_ir_free(probe);
    }
    if (object_for(triple, "ext_fn", "ext_data", "caller", &caller_data, &caller_size) ||
        definitions_for(triple, &defs_data, &defs_size))
        return 1;
    inputs[0].data = caller_data;
    inputs[0].size = caller_size;
    inputs[0].name = "caller";
    inputs[1].data = defs_data;
    inputs[1].size = defs_size;
    inputs[1].name = "definitions";
    if (snx_link(inputs, 2, exports, 2, "caller.so", &linked, error, sizeof(error)))
        return snx_error("PowerPC64 link: %s", error);
    eh = (const Elf64_Ehdr*)(const void*)linked.data;
    check(eh->e_machine == EM_PPC64 && eh->e_type == ET_DYN && snx_elf_ppc_abi(eh->e_flags) == SNX_ELF_PPC_ELFV2,
          "PowerPC64 module header");
    sh = (const Elf64_Shdr*)(const void*)(linked.data + eh->e_shoff);
    for (i = 0; i < eh->e_shnum; ++i)
        if (sh[i].sh_type == SHT_RELA) {
            const Elf64_Rela* r = (const Elf64_Rela*)(const void*)(linked.data + sh[i].sh_offset);
            size_t k;
            for (k = 0; k < sh[i].sh_size / sizeof(Elf64_Rela); ++k)
                ELF64_R_TYPE(r[k].r_info) == SNX_PPC_RELATIVE ? ++relatives : ++others;
        }
    check(relatives >= 1 && !others, "PowerPC64 module's dynamic relocations are R_PPC64_RELATIVE");
    caller = export_address(linked.data, "caller");
    callee = export_address(linked.data, "ext_fn");
    for (at = caller; caller && at < caller + 64 && !target; at += 4) {
        uint32_t insn = host32(linked.data + at);
        if ((insn & 0xFC000003u) == 0x48000001u) /* bl */
            target = at + (uint64_t)(int64_t)(int32_t)((insn & 0x03FFFFFCu) << 6) / 64;
    }
    check(callee && (target == callee || (target == callee + 8 && (host32(linked.data + callee) >> 16) == 0x3C4Cu &&
                                          (host32(linked.data + callee + 4) >> 16) == 0x3842u)),
          "PowerPC64 call to the callee's local entry");
    free(linked.data);
    free(caller_data);
    free(defs_data);
    return 0;
}

/* a guest function's tail call through a pointer, as snx_lookup's answer is called, which ir.c makes a stub of on
   PowerPC64: its object links alone, the stub falling back to the CPU's RtAPI.miss rather than a symbol of its own */
static int ppc64le_tail_call(void) {
    char error[1024];
    const char* exports[1] = {"guest"};
    LLVMTypeRef params[2], type;
    LLVMValueRef guest, call, args[2];
    unsigned char* data;
    size_t size;
    SnxLinkInput input;
    SnxLinkOutput linked;
    SnxIr* ir;
    if (snx_elf_host_data() != ELFDATA2LSB) return 0;
    LLVMInitializeAllAsmParsers(); /* the stub is inline assembly */
    ir = snx_ir_new_target("guest", '2', "powerpc64le-unknown-linux-gnu", error, sizeof(error));
    if (!ir) {
        printf("skipped the PowerPC64 tail call: %s\n", error);
        return 0;
    }
    params[0] = ir->t_ptr;
    params[1] = ir->t_i64;
    type = LLVMFunctionType(ir->t_void, params, 2, 0);
    guest = LLVMAddFunction(ir->mod, "guest", type);
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, guest, "entry"));
    args[0] = LLVMGetParam(guest, 0);
    args[1] = LLVMGetParam(guest, 1);
    call = LLVMBuildCall2(ir->b, type, LLVMBuildLoad2(ir->b, ir->t_ptr, args[0], "fn"), args, 2, "");
    LLVMSetTailCallKind(call, LLVMTailCallKindMustTail);
    LLVMBuildRetVoid(ir->b);
    if (snx_ir_object(ir, '2', &data, &size, error, sizeof(error))) return snx_error("PowerPC64 tail call: %s", error);
    snx_ir_free(ir);
    input.data = data;
    input.size = size;
    input.name = "guest";
    if (snx_link(&input, 1, exports, 1, "guest.so", &linked, error, sizeof(error))) {
        printf("FAIL: a PowerPC64 tail call through a pointer links alone: %s\n", error);
        ++failures;
    } else
        free(linked.data);
    free(data);
    return 0;
}

/* an AArch32 block going on to the next block, in another object, as module_arm.c's chain_block makes it: after the
   load of a hidden global the other object defines, a one-argument musttail call of a declaration, which ir.c makes a
   stub of on PowerPC64. The two link, and the block's jump lands on a branch to the next block's local entry */
static int ppc64le_block_chain(void) {
    char error[1024];
    const char* exports[2] = {"A_00201000", "A_00201008"};
    const char* triple = "powerpc64le-unknown-linux-gnu";
    unsigned char* data[2];
    size_t size[2], i;
    SnxLinkInput inputs[2];
    SnxLinkOutput linked;
    uint64_t block, next, at, stub = 0, target = 0;
    if (snx_elf_host_data() != ELFDATA2LSB) return 0;
    LLVMInitializeAllAsmParsers(); /* the stub is inline assembly */
    for (i = 0; i < 2; ++i) {
        SnxIr* ir = snx_ir_new_target(i ? "next" : "block", '2', triple, error, sizeof(error));
        LLVMTypeRef type;
        LLVMValueRef fn, global;
        if (!ir) {
            printf("skipped the PowerPC64 block chain: %s\n", error);
            return 0;
        }
        type = LLVMFunctionType(ir->t_void, &ir->t_ptr, 1, 0);
        global = LLVMAddGlobal(ir->mod, ir->t_i64, "snx_arm_poll_offset");
        LLVMSetVisibility(global, LLVMHiddenVisibility);
        fn = LLVMAddFunction(ir->mod, exports[i], type);
        LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, fn, "entry"));
        if (i) {
            LLVMSetInitializer(global, LLVMConstInt(ir->t_i64, 0, 0));
            LLVMBuildRetVoid(ir->b);
        } else {
            LLVMBasicBlockRef go = LLVMAppendBasicBlockInContext(ir->ctx, fn, "go"),
                              out = LLVMAppendBasicBlockInContext(ir->ctx, fn, "out");
            LLVMValueRef cpu = LLVMGetParam(fn, 0), off = LLVMBuildLoad2(ir->b, ir->t_i64, global, ""), call;
            LLVMValueRef poll = LLVMBuildLoad2(ir->b, ir->t_i32, LLVMBuildGEP2(ir->b, ir->t_i8, cpu, &off, 1, ""), "");
            LLVMBuildCondBr(ir->b, LLVMBuildICmp(ir->b, LLVMIntEQ, poll, LLVMConstInt(ir->t_i32, 0, 0), ""), go, out);
            LLVMPositionBuilderAtEnd(ir->b, go);
            call = LLVMBuildCall2(ir->b, type, LLVMAddFunction(ir->mod, exports[1], type), &cpu, 1, "");
            LLVMSetTailCallKind(call, LLVMTailCallKindMustTail);
            LLVMBuildRetVoid(ir->b);
            LLVMPositionBuilderAtEnd(ir->b, out);
            LLVMBuildRetVoid(ir->b);
        }
        if (snx_ir_object(ir, '2', &data[i], &size[i], error, sizeof(error)))
            return snx_error("PowerPC64 block chain: %s", error);
        snx_ir_free(ir);
        inputs[i].data = data[i];
        inputs[i].size = size[i];
        inputs[i].name = exports[i];
    }
    if (snx_link(inputs, 2, exports, 2, "block.so", &linked, error, sizeof(error))) {
        printf("FAIL: a PowerPC64 block's tail call of the next block links: %s\n", error);
        ++failures;
    } else {
        block = export_address(linked.data, exports[0]);
        next = export_address(linked.data, exports[1]);
        for (at = block; block && at < block + 64 && !stub; at += 4) {
            uint32_t insn = host32(linked.data + at);
            if ((insn & 0xFC000003u) == 0x48000000u) /* b */
                stub = at + (uint64_t)(int64_t)(int32_t)((insn & 0x03FFFFFCu) << 6) / 64;
        }
        if (stub && (host32(linked.data + stub) & 0xFC000003u) == 0x48000000u)
            target = stub + (uint64_t)(int64_t)(int32_t)((host32(linked.data + stub) & 0x03FFFFFCu) << 6) / 64;
        check(next && (target == next || (target == next + 8 && (host32(linked.data + next) >> 16) == 0x3C4Cu)),
              "PowerPC64 block's jump to its stub, a branch to the next block's local entry");
        free(linked.data);
    }
    free(data[0]);
    free(data[1]);
    return 0;
}

/* Where caller's first call and first GOT load go, decoded from the module: RISC-V's AUIPC with the JALR or LD after
   it (compressed instructions skipped by their length), LoongArch's PCADDU18I with JIRL and PCALAU12I with LD.D */
static void decode_caller(unsigned machine, const unsigned char* module, uint64_t caller, uint64_t* call,
                          uint64_t* slot) {
    uint64_t at = caller, hi_at = 0, hi = 0;
    unsigned hi_rd = 0;
    *call = *slot = 0;
    while (at < caller + 96 && (!*call || !*slot)) {
        uint32_t insn = get32(module + at);
        if (machine == EM_RISCV) {
            unsigned rd = insn >> 7 & 31, rs1 = insn >> 15 & 31;
            uint64_t lo = (uint64_t)sign(insn >> 20, 12);
            if ((insn & 3) != 3) { /* compressed */
                at += 2;
                continue;
            }
            if ((insn & 0x7F) == 0x17) { /* AUIPC */
                hi_at = at;
                hi = (uint64_t)sign(insn & 0xFFFFF000u, 32);
                hi_rd = rd;
            } else if (hi_at && rs1 == hi_rd && (insn & 0x707F) == 0x67 && !*call) /* JALR */
                *call = hi_at + hi + lo;
            else if (hi_at && rs1 == hi_rd && (insn & 0x707F) == 0x3003 && !*slot) /* LD */
                *slot = hi_at + hi + lo;
        } else {
            unsigned rd = insn & 31, rj = insn >> 5 & 31;
            if (insn >> 25 == 0x0D || insn >> 25 == 0x0F) { /* PCALAU12I, PCADDU18I */
                hi_at = at;
                hi = insn >> 25 == 0x0D ? (at & ~(uint64_t)0xFFF) + (uint64_t)(sign(insn >> 5 & 0xFFFFF, 20) * 4096)
                                        : at + (uint64_t)(sign(insn >> 5 & 0xFFFFF, 20) * 262144);
                hi_rd = rd;
            } else if (hi_at && rj == hi_rd && insn >> 26 == 0x13 && !*call) /* JIRL */
                *call = hi + (uint64_t)(sign(insn >> 10 & 0xFFFF, 16) * 4);
            else if (hi_at && rj == hi_rd && insn >> 22 == 0xA3 && !*slot) /* LD.D */
                *slot = hi + (uint64_t)sign(insn >> 10 & 0xFFF, 12);
        }
        at += 4;
    }
}

/* riscv64 and loongarch64 objects linked into a module: its header repeats theirs (the float ABI), its dynamic
   relocations are only the machine's RELATIVE, the call reaches ext_fn and the GOT load a slot that holds ext_data.
   Linked in both orders, which moves the code and not the GOT: one layout alone can put a displacement's low 12 bits
   where the AUIPC's own are, and hide a RISC-V low half written from the wrong value */
static int other_objects(const char* triple, unsigned machine, const char* what) {
    char error[1024], name[128];
    const char* exports[3] = {"caller", "ext_fn", "ext_data"};
    unsigned char *caller_data, *defs_data;
    size_t caller_size, defs_size, i;
    SnxLinkInput inputs[2];
    SnxLinkOutput linked;
    const Elf64_Ehdr* eh;
    const Elf64_Shdr* sh;
    uint64_t call, slot, data;
    int order;
    {
        SnxIr* probe = snx_ir_new_target("probe", '2', triple, error, sizeof(error));
        if (!probe) {
            printf("skipped the %s objects: %s\n", what, error);
            return 0;
        }
        snx_ir_free(probe);
    }
    if (object_for(triple, "ext_fn", "ext_data", "caller", &caller_data, &caller_size) ||
        definitions_for(triple, &defs_data, &defs_size))
        return 1;
    for (order = 0; order < 2; ++order) {
        size_t relatives = 0, others = 0;
        const char* which = order ? "definitions first" : "caller first";
        inputs[order].data = caller_data;
        inputs[order].size = caller_size;
        inputs[order].name = "caller";
        inputs[!order].data = defs_data;
        inputs[!order].size = defs_size;
        inputs[!order].name = "definitions";
        if (snx_link(inputs, 2, exports, 3, "caller.so", &linked, error, sizeof(error)))
            return snx_error("%s link: %s", what, error);
        eh = (const Elf64_Ehdr*)(const void*)linked.data;
        snprintf(name, sizeof name, "%s module header, the objects' e_flags (%s)", what, which);
        check(eh->e_machine == machine && eh->e_type == ET_DYN &&
                  eh->e_flags == ((const Elf64_Ehdr*)(const void*)caller_data)->e_flags,
              name);
        sh = (const Elf64_Shdr*)(const void*)(linked.data + eh->e_shoff);
        for (i = 0; i < eh->e_shnum; ++i)
            if (sh[i].sh_type == SHT_RELA) {
                const Elf64_Rela* r = (const Elf64_Rela*)(const void*)(linked.data + sh[i].sh_offset);
                size_t k;
                for (k = 0; k < sh[i].sh_size / sizeof(Elf64_Rela); ++k)
                    ELF64_R_TYPE(r[k].r_info) == snx_elf_relative_type(machine) ? ++relatives : ++others;
            }
        snprintf(name, sizeof name, "%s module's dynamic relocations are RELATIVE (%s)", what, which);
        check(relatives >= 1 && !others, name);
        decode_caller(machine, linked.data, export_address(linked.data, "caller"), &call, &slot);
        data = export_address(linked.data, "ext_data");
        snprintf(name, sizeof name, "%s call to ext_fn (%s)", what, which);
        check(call && call == export_address(linked.data, "ext_fn"), name);
        snprintf(name, sizeof name, "%s GOT slot of ext_data (%s)", what, which);
        check(slot && data && slot + 8 <= linked.size && get64(linked.data + slot) == data, name);
        free(linked.data);
    }
    free(caller_data);
    free(defs_data);
    return 0;
}
#endif

int main(void) {
    data_relocations();
    instruction_relocations();
    veneers();
    ppc_relocations();
    riscv_relocations();
    loongarch_relocations();
    need_too_large();
    if (llvm_objects()) ++failures;
#ifndef __APPLE__ /* AOT modules, which macOS doesn't link as ELF (link_elf.c) */
    if (ppc64le_objects() || ppc64le_tail_call() || ppc64le_block_chain()) ++failures;
    if (other_objects("riscv64-unknown-linux-gnu", EM_RISCV, "RISC-V") ||
        other_objects("loongarch64-unknown-linux-gnu", EM_LOONGARCH, "LoongArch"))
        ++failures;
#endif
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
