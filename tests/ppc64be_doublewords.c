/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A big-endian PowerPC64's guest doublewords (tools/sailgen/ppc64.h's rk_gldst): LDBRX and STDBRX where the host has
   them (ISA 2.06, the Cell: SGX_F_LDBRX), two words each elsewhere. A block of the 64-bit loads and stores of every
   form (an offset, a register, unaligned, a pair) and a 32-bit one, the back end's code made both ways, runs as a
   program of its own under qemu-ppc64, on the CPUs before ISA 2.06 (the 970, the e5500) and after it (POWER7): every
   register and the guest's memory are what A64 says, the code without the feature has neither instruction and runs
   on every CPU, and the code with it is SIGILL on a 970, which shows the CPU is one without them.
     snx-test-ppc64be-doublewords QEMU FOLDER
   QEMU: qemu-ppc64's path (77, skipped, where there is none); FOLDER: where the programs are written */
#include "sail_mach_emit.h"
#define SNX_TEST_SWAP_CPU 1
#include "support/swap_state.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/aarch64/cpu.h"
#include "common/platform/file.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>

enum { SGX_F_LDBRX = 1 }; /* ppc64.h's */

/* the program's one segment: its code, the block's, the CPU structure, guest memory, what the block returned, the
   stack */
#define BASE   0x10000000u
#define STUB   0x1000u
#define BLOCK  0x2000u
#define STATE  0x10000u
#define MEMORY 0x20000u
#define RESULT 0x30000u
#define STACK  0x40000u
#define END    0x50000u
#define GUEST  0x100u /* x1: where the block's accesses are, in guest memory */
#define SHOWN  0x200u /* the guest memory written out */

/* LDR X0, [X1]; LDR X2, [X1, #8]; STR X3, [X1, #16]; LDR X4, [X1, X5]; STR X6, [X1, X5]; LDUR X7, [X1, #3];
   LDR W8, [X1, #4]; LDP X9, X10, [X1, #32]; STP X11, X12, [X1, #48] */
static const uint32_t k_words[] = {0xF9400020u, 0xF9400422u, 0xF9000823u, 0xF8656824u, 0xF8256826u,
                                   0xF8403027u, 0xB9400428u, 0xA9422829u, 0xA903302Bu};
enum { WORDS = sizeof k_words / sizeof *k_words };

static struct {
    int failures;
    unsigned char image[END];
    unsigned char code[2][16384]; /* the block without LDBRX and with it */
} g_test;
static void check(int ok, const char* what, const char* of) {
    if (ok) return;
    ++g_test.failures;
    fprintf(stderr, "FAIL: %s: %s\n", of, what);
}

static void be32(unsigned char* p, uint32_t v) {
    for (int k = 0; k < 4; ++k) p[k] = (unsigned char)(v >> (24 - 8 * k));
}
static void be16(unsigned char* p, uint32_t v) { p[0] = (unsigned char)(v >> 8), p[1] = (unsigned char)v; }
static void be64(unsigned char* p, uint64_t v) {
    be32(p, (uint32_t)(v >> 32));
    be32(p + 4, (uint32_t)v);
}
static uint64_t le64(const unsigned char* p) {
    uint64_t v = 0;
    for (int k = 7; k >= 0; --k) v = v << 8 | p[k];
    return v;
}
static void put_le64(unsigned char* p, uint64_t v) {
    for (int k = 0; k < 8; ++k) p[k] = (unsigned char)(v >> (8 * k));
}

/* the program's stub, Power ISA's D, X and SC forms written here (llvm-mc reads them back as named): the stack, the
   CPU's address in r3, the block called through CTR with its address in r12 (ELFv2's global entry), what it returns
   stored, then the state, the guest memory and the return written to stdout, and exit */
static size_t stub(unsigned char* p) {
    static const uint32_t words[] = {
        0x3C201004u, /* lis r1, 0x1004 */
        0x6021FF00u, /* ori r1, r1, 0xff00: below the segment's end, where the block saves LR at 16(r1) */
        0x3C601001u, /* lis r3, 0x1001: the state */
        0x3D801000u, /* lis r12, 0x1000 */
        0x618C2000u, /* ori r12, r12, 0x2000: the block */
        0x7D8903A6u, /* mtctr r12 */
        0x4E800421u, /* bctrl */
        0x3C801003u, /* lis r4, 0x1003 */
        0xF8640000u, /* std r3, 0(r4): what the block returned */
        0x38000004u, /* li r0, 4: write */
        0x38600001u, /* li r3, 1 */
        0x3C801001u, /* lis r4, 0x1001 */
        0x38A00000u, /* li r5, size of the state (below) */
        0x44000002u, /* sc */
        0x38000004u, /* li r0, 4 */
        0x38600001u, /* li r3, 1 */
        0x3C801002u, /* lis r4, 0x1002: guest memory */
        0x38A00200u, /* li r5, SHOWN */
        0x44000002u, /* sc */
        0x38000004u, /* li r0, 4 */
        0x38600001u, /* li r3, 1 */
        0x3C801003u, /* lis r4, 0x1003 */
        0x38A00008u, /* li r5, 8 */
        0x44000002u, /* sc */
        0x380000EAu, /* li r0, 234: exit_group */
        0x38600000u, /* li r3, 0 */
        0x44000002u, /* sc */
    };
    for (size_t k = 0; k < sizeof words / sizeof *words; ++k) be32(p + 4 * k, words[k]);
    be16(p + 12 * 4 + 2, (uint32_t)sizeof(CPU)); /* li r5's immediate */
    return sizeof words;
}

/* the CPU before the block: x1 the accesses' address, x5 an index, x3, x6, x11 and x12 stored; guest memory a
   pattern */
static void start(CPU* c, unsigned char* memory) {
    memset(c, 0, sizeof *c);
    c->x[1] = GUEST, c->x[5] = 0x40;
    c->x[3] = 0x1122334455667788ull, c->x[6] = 0x99AABBCCDDEEFF00ull;
    c->x[11] = 0x0123456789ABCDEFull, c->x[12] = 0xFEDCBA9876543210ull;
    c->mem = (u8*)(uintptr_t)(BASE + MEMORY);
    for (unsigned k = 0; k < SHOWN; ++k) memory[k] = (unsigned char)(k * 7 + 1);
}
/* what A64 makes of the words, in the same order */
static void expect(CPU* c, unsigned char* m) {
    const u64 a = GUEST;
    c->x[0] = le64(m + a), c->x[2] = le64(m + a + 8);
    put_le64(m + a + 16, c->x[3]);
    c->x[4] = le64(m + a + 0x40);
    put_le64(m + a + 0x40, c->x[6]);
    c->x[7] = le64(m + a + 3);
    c->x[8] = (u32)le64(m + a + 4);
    c->x[9] = le64(m + a + 32), c->x[10] = le64(m + a + 40);
    put_le64(m + a + 48, c->x[11]);
    put_le64(m + a + 56, c->x[12]);
}

/* the block's code for a host of these features (its big-endian words) and whether it has LDBRX or STDBRX: its size */
static size_t block(unsigned features, unsigned char* code, size_t cap, int* reversed) {
    size_t count = WORDS, size = snx_mach_block(k_words, &count, 0x1000, SNX_A64_TARGET_OUNCE, features, code, cap);
    *reversed = 0;
    if (count != WORDS) return 0;
    for (size_t k = 0; k + 4 <= size; k += 4) {
        uint32_t w = (uint32_t)code[k] << 24 | (uint32_t)code[k + 1] << 16 | (uint32_t)code[k + 2] << 8 | code[k + 3];
        *reversed |= (w & 0xFC0007FEu) == 0x7C000428u || (w & 0xFC0007FEu) == 0x7C000528u; /* LDBRX, STDBRX */
    }
    return size;
}

/* the program of a block in path, executable */
static int write_program(const char* path, const unsigned char* code, size_t size) {
    unsigned char* p = g_test.image;
    CPU c;
    memset(p, 0, sizeof g_test.image);
    if (STUB + stub(p + STUB) > BLOCK || BLOCK + size > STATE) return -1;
    memcpy(p + BLOCK, code, size);
    start(&c, p + MEMORY);
    snx_test_swap_cpu(&c); /* the host's numbers, the program's in big-endian order */
    memcpy(p + STATE, &c, sizeof c);
    /* ELF64, big-endian, PowerPC64 (21), ELFv2 (e_flags 2): one PT_LOAD of every byte, read, written and run */
    memcpy(p,
           "\x7f"
           "ELF\x02\x02\x01",
           7);
    be16(p + 16, 2), be16(p + 18, 21), be32(p + 20, 1);
    be64(p + 24, BASE + STUB), be64(p + 32, 64), be64(p + 40, 0);
    be32(p + 48, 2), be16(p + 52, 64), be16(p + 54, 56), be16(p + 56, 1);
    be32(p + 64, 1), be32(p + 68, 7), be64(p + 72, 0), be64(p + 80, BASE), be64(p + 88, BASE);
    be64(p + 96, END), be64(p + 104, END), be64(p + 112, 0x10000);
    return snx_write_file(path, p, END) || chmod(path, 0755) ? -1 : 0;
}

/* the program run by qemu as cpu: its exit, and the state, guest memory and next pc it wrote into c, memory, *next */
static int run(const char* qemu, const char* cpu, const char* program, CPU* c, unsigned char* memory, u64* next) {
    char* command = snx_format("'%s' -cpu %s '%s' 2>/dev/null", qemu, cpu, program);
    unsigned char out[sizeof(CPU) + SHOWN + 8];
    size_t got = 0, n;
    FILE* f = command ? popen(command, "r") : NULL;
    int status;
    while (f && (n = fread(out + got, 1, sizeof out - got, f)) > 0) got += n;
    status = f ? pclose(f) : -1;
    free(command);
    if (got != sizeof out) return status ? status : -1;
    memcpy(c, out, sizeof *c);
    snx_test_swap_cpu(c);
    memcpy(memory, out + sizeof *c, SHOWN);
    *next = 0;
    for (int k = 0; k < 8; ++k) *next = *next << 8 | out[sizeof *c + SHOWN + k];
    return status;
}

/* the run of features' code on cpu: every register and guest memory as A64's, the next pc past the words */
static void holds(const char* qemu, const char* cpu, unsigned features, const char* program) {
    CPU c, want;
    unsigned char memory[SHOWN], want_memory[SHOWN];
    u64 next = 0;
    char of[96];
    snprintf(of, sizeof of, "the code %s LDBRX on a %s", features & SGX_F_LDBRX ? "with" : "without", cpu);
    int status = run(qemu, cpu, program, &c, memory, &next);
    check(status == 0, "runs to its end", of);
    if (status) return;
    start(&want, want_memory);
    expect(&want, want_memory);
    for (int r = 0; r < 13; ++r)
        if (c.x[r] != want.x[r]) {
            char what[96];
            snprintf(what, sizeof what, "x%d is %016llx, not %016llx", r, (unsigned long long)c.x[r],
                     (unsigned long long)want.x[r]);
            check(0, what, of);
        }
    check(!memcmp(memory, want_memory, SHOWN), "guest memory as A64's", of);
    check(next == 0x1000 + 4 * WORDS, "the next pc", of);
}

int main(int argc, char** argv) {
    static const char* const before[] = {"970", "e5500"}; /* without LDBRX and STDBRX: qemu's, POWER4's and Book E's */
    size_t size[2];
    int reversed[2];
    char* programs[2];
    if (argc != 3) {
        fprintf(stderr, "usage: snx-test-ppc64be-doublewords QEMU FOLDER\n");
        return 2;
    }
    for (int f = 0; f < 2; ++f) {
        size[f] = block(f ? SGX_F_LDBRX : 0, g_test.code[f], sizeof g_test.code[f], &reversed[f]);
        check(size[f] != 0, "the back end makes the block", f ? "with LDBRX" : "without LDBRX");
        check(reversed[f] == f, f ? "LDBRX or STDBRX in it" : "neither LDBRX nor STDBRX in it",
              f ? "with LDBRX" : "without LDBRX");
    }
    if (g_test.failures) return 1;
    if (!snx_is_file(argv[1])) {
        printf("ppc64be_doublewords: no %s to run the code: the code alone was checked\n", argv[1]);
        return 77;
    }
    if (snx_mkdirs(argv[2])) return 1;
    for (int f = 0; f < 2; ++f) {
        programs[f] = snx_path_join(argv[2], f ? "with-ldbrx" : "without-ldbrx");
        check(programs[f] && !write_program(programs[f], g_test.code[f], size[f]), "written", programs[f]);
    }
    if (g_test.failures) return 1;
    for (size_t k = 0; k < sizeof before / sizeof *before; ++k) holds(argv[1], before[k], 0, programs[0]);
    holds(argv[1], "power7", 0, programs[0]);
    holds(argv[1], "power7", SGX_F_LDBRX, programs[1]);
    {
        CPU c;
        unsigned char memory[SHOWN];
        u64 next;
        int status = run(argv[1], "970", programs[1], &c, memory, &next);
        check(status != 0, "the code with LDBRX is refused on a 970: the CPU has none", "qemu's 970");
    }
    free(programs[0]), free(programs[1]);
    printf("ppc64be_doublewords: %d failures\n", g_test.failures);
    return g_test.failures != 0;
}
