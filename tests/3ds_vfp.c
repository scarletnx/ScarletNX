/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ARM11 VFP short vectors through the real dispatcher: checked blocks, the SnxArmRt.undefined backend, CreateThread
   and the scheduler */
#include "3ds/aot/ctr_module.h"
#include "3ds/memory/host.h"
#include "3ds/runtime/internal.h"
#include "common/cpu/arm/exec.h"
#include "common/utilities/endian.h"
#include <stdio.h>

/* round to zero, flush to zero and default NaN, as a new thread starts */
#define FPSCR_DEFAULT      0x03c00000u
#define FPSCR_LEN(n)       ((uint32_t)(n) << 16)
#define FPSCR_STRIDE(bits) ((uint32_t)(bits) << 20)
#define FPSCR_AHP          (1u << 26)
#define FPSCR_QC           (1u << 27)
#define FPSCR_IDE          (1u << 15) /* an enabled exception */

static unsigned failures;

static void check(int ok, const char* what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        ++failures;
    }
}

/* A NULL block means the compiler or the loader failed before any guest instruction ran. */
static int compiled(SnxArmBlock block, const char* what) {
    if (block) return 1;
    printf("FAIL: %s block is NULL (compiler or loader)\n", what);
    ++failures;
    return 0;
}

/* The words are guest little-endian bytes at the image's text base, as the
   dispatcher fetches them. */
static SnxCtrProcess* make_process(const uint32_t* words, size_t count, uint32_t fpscr) {
    unsigned char code[0x3000] = {0};
    for (size_t i = 0; i < count; ++i) snx_st_le32(code + i * 4, words[i]);
    SnxCtrImage image = {.segment = {{0x100000, 1, 16}, {0x200000, 1, 4}, {0x300000, 1, 4}},
                         .stack_size = 0x4000,
                         .main_priority = 0x30,
                         .bss_size = 4096,
                         .code = code,
                         .code_size = sizeof code};
    SnxCtrProcess* p = snx_ctr_process_new(&image, SNX_CTR_OLD);
    if (p) p->cpu.state.cpu.fpscr = fpscr;
    return p;
}

static SnxArmBlock compile(SnxArmExec* exec, const uint32_t* words, size_t count, SnxArmProfile profile, bool checked) {
    size_t consumed = 0;
    return snx_arm_exec_compile(exec, words, count, 0x100000, 0, profile, checked, &consumed);
}

/* One compiled block at 0x100000 and nothing else in the table: a block that
   stops with an exit ends the dispatch; one that runs the word spends the
   one-block budget after the backend returns. */
static SnxCtrRunResult run(SnxCtrProcess* p, SnxArmBlock block) {
    SnxArmBlock arm[1024] = {block}, thumb[2048] = {0};
    SnxCtrModule module = {.text_address = 0x100000, .text_bytes = 4096, .arm_blocks = arm, .thumb_blocks = thumb};
    uint64_t executed = 0;
    return snx_ctr_dispatch(&module, p, 1, &executed);
}

/* S16..S23 = 1, 2, 3, 4, 10, 20, 30, 40 */
static void load_f32_sources(uint64_t* d) {
    d[8] = 0x400000003f800000ull;
    d[9] = 0x4080000040400000ull;
    d[10] = 0x41a0000041200000ull;
    d[11] = 0x4220000041f00000ull;
}

/* A checked block without a VFP word compiles, loads and runs. */
static void scalar_block(SnxArmExec* exec) {
    static const uint32_t words[] = {0xe1a00000u, 0xeafffffeu}; /* MOV r0,r0; B . */
    SnxCtrProcess* p = make_process(words, 2, FPSCR_DEFAULT);
    check(p != NULL, "scalar process");
    if (!p) return;
    SnxArmCPU* cpu = &p->cpu.state.cpu;
    cpu->r[0] = 0x12345678u;
    SnxArmBlock block = compile(exec, words, 2, SNX_ARM_PROFILE_ARM11, true);
    if (compiled(block, "scalar")) {
        check(run(p, block) == SNX_CTR_RUN_BUDGET, "the checked scalar block runs");
        check(cpu->r[0] == 0x12345678u, "the checked scalar block leaves r0 alone");
    }
    snx_ctr_process_free(p);
}

/* VADD.F32 S8,S16,S20, LEN=3, STRIDE=1: S8..S11 = S16..S19 + S20..S23. The
   handled instruction returns with r15 set to the next PC (0x100004), which
   points at the following branch; the assertion verifies this continuation PC,
   not that the branch executes. */
static void vector_f32(SnxArmExec* exec) {
    static const uint32_t words[] = {0xee384a0au, 0xeafffffeu};
    SnxCtrProcess* p = make_process(words, 2, FPSCR_DEFAULT | FPSCR_LEN(3));
    check(p != NULL, "f32 process");
    if (!p) return;
    SnxArmCPU* cpu = &p->cpu.state.cpu;
    load_f32_sources(cpu->d);
    SnxArmBlock block = compile(exec, words, 2, SNX_ARM_PROFILE_ARM11, true);
    if (compiled(block, "f32")) {
        check(run(p, block) == SNX_CTR_RUN_BUDGET, "f32 short vector runs");
        check(cpu->d[4] == 0x41b0000041300000ull, "f32 S8..S9 = 11,22");
        check(cpu->d[5] == 0x4230000042040000ull, "f32 S10..S11 = 33,44");
        check(cpu->r[15] == 0x100004u, "the handled word continues to the block's own branch");
    }
    snx_ctr_process_free(p);
}

/* VMUL.F32 S8,S16,S20, LEN=3, STRIDE=1: a second arithmetic class, with exact
   integer products so the expected lanes stay independent literals. */
static void vector_mul(SnxArmExec* exec) {
    static const uint32_t words[] = {0xee284a0au, 0xeafffffeu};
    SnxCtrProcess* p = make_process(words, 2, FPSCR_DEFAULT | FPSCR_LEN(3));
    check(p != NULL, "mul process");
    if (!p) return;
    SnxArmCPU* cpu = &p->cpu.state.cpu;
    load_f32_sources(cpu->d);
    SnxArmBlock block = compile(exec, words, 2, SNX_ARM_PROFILE_ARM11, true);
    if (compiled(block, "mul")) {
        check(run(p, block) == SNX_CTR_RUN_BUDGET, "VMUL short vector runs");
        check(cpu->d[4] == 0x4220000041200000ull, "VMUL S8..S9 = 10,40");
        check(cpu->d[5] == 0x4320000042b40000ull, "VMUL S10..S11 = 90,160");
    }
    snx_ctr_process_free(p);
}

/* VADD.F64 D6,D8,D9, LEN=3, STRIDE=1: the destination bank rotates
   D6,D7,D4,D5 and the second source rotates D9,D10,D11,D8. */
static void vector_f64(SnxArmExec* exec) {
    static const uint32_t words[] = {0xee386b09u, 0xeafffffeu};
    SnxCtrProcess* p = make_process(words, 2, FPSCR_DEFAULT | FPSCR_LEN(3));
    check(p != NULL, "f64 process");
    if (!p) return;
    SnxArmCPU* cpu = &p->cpu.state.cpu;
    cpu->d[2] = 0x1111111122222222ull;  /* the first bank stays untouched */
    cpu->d[8] = 0x3ff8000000000000ull;  /* 1.5 */
    cpu->d[9] = 0x4010000000000000ull;  /* 4.0 */
    cpu->d[10] = 0x4000000000000000ull; /* 2.0 */
    cpu->d[11] = 0x4014000000000000ull; /* 5.0 */
    SnxArmBlock block = compile(exec, words, 2, SNX_ARM_PROFILE_ARM11, true);
    if (compiled(block, "f64")) {
        check(run(p, block) == SNX_CTR_RUN_BUDGET, "f64 short vector runs");
        check(cpu->d[6] == 0x4016000000000000ull, "f64 D6 = 5.5");
        check(cpu->d[7] == 0x4018000000000000ull, "f64 D7 = 6.0");
        check(cpu->d[4] == 0x401c000000000000ull, "f64 D4 = 7.0 after the bank rotates");
        check(cpu->d[5] == 0x401a000000000000ull, "f64 D5 = 6.5 after the bank rotates");
        check(cpu->d[2] == 0x1111111122222222ull, "the first bank stays untouched");
    }
    snx_ctr_process_free(p);
}

/* VADD.F32 S0,S1,S2, LEN=1: a destination in the first bank is a scalar
   operation, so only S0 changes; S1, S2 and S3 keep their lanes. */
static void bank_zero(SnxArmExec* exec) {
    static const uint32_t words[] = {0xee300a81u, 0xeafffffeu};
    SnxCtrProcess* p = make_process(words, 2, FPSCR_DEFAULT | FPSCR_LEN(1));
    check(p != NULL, "bank-zero process");
    if (!p) return;
    SnxArmCPU* cpu = &p->cpu.state.cpu;
    cpu->d[0] = 0x400000003f800000ull; /* S0 = 1.0, S1 = 2.0 */
    cpu->d[1] = 0x4110000040a00000ull; /* S2 = 5.0, S3 = 9.0 */
    SnxArmBlock block = compile(exec, words, 2, SNX_ARM_PROFILE_ARM11, true);
    if (compiled(block, "bank-zero")) {
        check(run(p, block) == SNX_CTR_RUN_BUDGET, "the first-bank destination runs as a scalar");
        check(cpu->d[0] == 0x4000000040e00000ull, "S0 = 7.0 and S1 keeps 2.0");
        check(cpu->d[1] == 0x4110000040a00000ull, "S2 and S3 keep their lanes");
    }
    snx_ctr_process_free(p);
}

/* LEN=0: the emitter's own scalar path, untouched by the short-vector backend. */
static void scalar_len0(SnxArmExec* exec) {
    static const uint32_t words[] = {0xee384a0au, 0xeafffffeu};
    SnxCtrProcess* p = make_process(words, 2, FPSCR_DEFAULT);
    check(p != NULL, "LEN=0 process");
    if (!p) return;
    SnxArmCPU* cpu = &p->cpu.state.cpu;
    cpu->d[4] = 0x1111111122222222ull;  /* S8 and S9 sentinels */
    cpu->d[5] = 0x3333333344444444ull;  /* S10 and S11 sentinels */
    cpu->d[8] = 0x000000003f800000ull;  /* S16 = 1.0 */
    cpu->d[10] = 0x0000000040400000ull; /* S20 = 3.0 */
    SnxArmBlock block = compile(exec, words, 2, SNX_ARM_PROFILE_ARM11, true);
    if (compiled(block, "LEN=0")) {
        check(run(p, block) == SNX_CTR_RUN_BUDGET, "LEN=0 keeps the emitter's scalar path");
        check(cpu->d[4] == 0x1111111140800000ull, "LEN=0 S8 = 4.0 and S9 keeps its lane");
        check(cpu->d[5] == 0x3333333344444444ull, "LEN=0 leaves the next pair alone");
    }
    snx_ctr_process_free(p);
}

/* VADD.F32 S8,S16,S20, LEN=3, STRIDE=2: both sources stay in the 32-byte bank
   that holds S16..S23. N steps S16,S18,S20,S22; M starts at S20 and wraps to
   S16,S18, so S12 = S20 + S16 and S14 = S22 + S18. The S24/S26 sentinels are
   outside the source bank: a linear-source reading would give 13 and 24. */
static void stride2_f32(SnxArmExec* exec) {
    static const uint32_t words[] = {0xee384a0au, 0xeafffffeu};
    SnxCtrProcess* p = make_process(words, 2, FPSCR_DEFAULT | FPSCR_LEN(3) | FPSCR_STRIDE(3));
    check(p != NULL, "stride2 f32 process");
    if (!p) return;
    SnxArmCPU* cpu = &p->cpu.state.cpu;
    cpu->d[4] = 0x1111111122222222ull; /* S8..S15 sentinels */
    cpu->d[5] = 0x1111111122222222ull;
    cpu->d[6] = 0x1111111122222222ull;
    cpu->d[7] = 0x1111111122222222ull;
    cpu->d[8] = 0x000000003f800000ull;  /* S16 = 1.0 */
    cpu->d[9] = 0x0000000040000000ull;  /* S18 = 2.0 */
    cpu->d[10] = 0x0000000040400000ull; /* S20 = 3.0 */
    cpu->d[11] = 0x0000000040800000ull; /* S22 = 4.0 */
    cpu->d[12] = 0x0000000041200000ull; /* S24 = 10.0, outside the source bank */
    cpu->d[13] = 0x0000000041a00000ull; /* S26 = 20.0, outside the source bank */
    SnxArmBlock block = compile(exec, words, 2, SNX_ARM_PROFILE_ARM11, true);
    if (compiled(block, "stride2 f32")) {
        check(run(p, block) == SNX_CTR_RUN_BUDGET, "STRIDE=2 single short vector runs");
        check(cpu->d[4] == 0x1111111140800000ull, "stride2 S8 = 4.0 and S9 keeps its lane");
        check(cpu->d[5] == 0x1111111140c00000ull, "stride2 S10 = 6.0 and S11 keeps its lane");
        check(cpu->d[6] == 0x1111111140800000ull, "stride2 S12 = 4.0 after the second source wraps");
        check(cpu->d[7] == 0x1111111140c00000ull, "stride2 S14 = 6.0 after the second source wraps");
        check(cpu->d[8] == 0x000000003f800000ull && cpu->d[11] == 0x0000000040800000ull,
              "stride2 leaves the source bank alone");
        check(cpu->d[12] == 0x0000000041200000ull && cpu->d[13] == 0x0000000041a00000ull,
              "stride2 leaves the out-of-bank sentinels alone");
    }
    snx_ctr_process_free(p);
}

/* VADD.F64 D6,D8,D9, LEN=1, STRIDE=2: D6 = D8 + D9 and D4 = D10 + D11. */
static void stride2_f64(SnxArmExec* exec) {
    static const uint32_t words[] = {0xee386b09u, 0xeafffffeu};
    SnxCtrProcess* p = make_process(words, 2, FPSCR_DEFAULT | FPSCR_LEN(1) | FPSCR_STRIDE(3));
    check(p != NULL, "stride2 f64 process");
    if (!p) return;
    SnxArmCPU* cpu = &p->cpu.state.cpu;
    cpu->d[2] = 0x1111111122222222ull;
    cpu->d[4] = 0x3333333344444444ull;
    cpu->d[5] = 0x5555555566666666ull;
    cpu->d[6] = 0x7777777788888888ull;
    cpu->d[7] = 0x99999999aaaaaaaaull;
    cpu->d[8] = 0x3ff8000000000000ull;  /* 1.5 */
    cpu->d[9] = 0x4010000000000000ull;  /* 4.0 */
    cpu->d[10] = 0x4000000000000000ull; /* 2.0 */
    cpu->d[11] = 0x4014000000000000ull; /* 5.0 */
    SnxArmBlock block = compile(exec, words, 2, SNX_ARM_PROFILE_ARM11, true);
    if (compiled(block, "stride2 f64")) {
        check(run(p, block) == SNX_CTR_RUN_BUDGET, "STRIDE=2 double short vector runs");
        check(cpu->d[6] == 0x4016000000000000ull, "stride2 D6 = 5.5");
        check(cpu->d[4] == 0x401c000000000000ull, "stride2 D4 = 7.0 after the bank rotates");
        check(cpu->d[5] == 0x5555555566666666ull && cpu->d[7] == 0x99999999aaaaaaaaull,
              "stride2 leaves D5 and D7 alone");
        check(cpu->d[2] == 0x1111111122222222ull, "stride2 leaves the first bank alone");
    }
    snx_ctr_process_free(p);
}

/* STRIDE 01 and 10 are reserved: the word must stop unsupported and write
   nothing, with FPSCR unchanged. */
static void invalid_stride(SnxArmExec* exec) {
    static const uint32_t words[] = {0xee384a0au, 0xeafffffeu};
    static const struct {
        const char* name;
        uint32_t fpscr;
    } cases[] = {{"STRIDE 01", FPSCR_DEFAULT | FPSCR_LEN(1) | FPSCR_STRIDE(1)},
                 {"STRIDE 10", FPSCR_DEFAULT | FPSCR_LEN(1) | FPSCR_STRIDE(2)}};
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        SnxCtrProcess* p = make_process(words, 2, cases[i].fpscr);
        char label[64];
        snprintf(label, sizeof label, "%s fail closed", cases[i].name);
        check(p != NULL, "invalid-stride process");
        if (!p) continue;
        SnxArmCPU* cpu = &p->cpu.state.cpu;
        cpu->d[4] = 0x1111111122222222ull;
        SnxArmBlock block = compile(exec, words, 2, SNX_ARM_PROFILE_ARM11, true);
        if (compiled(block, cases[i].name)) {
            check(run(p, block) == SNX_CTR_RUN_UNSUPPORTED, label);
            check(cpu->d[4] == 0x1111111122222222ull, "an invalid stride leaves the destination alone");
            check(cpu->fpscr == cases[i].fpscr, "an invalid stride leaves FPSCR alone");
        }
        snx_ctr_process_free(p);
    }
}

/* LEN=3, STRIDE=2 on a double bank asks for 8 registers of a 4-register bank:
   the length limit fails closed before any lane is written. */
static void overlength(SnxArmExec* exec) {
    static const uint32_t words[] = {0xee386b09u, 0xeafffffeu};
    SnxCtrProcess* p = make_process(words, 2, FPSCR_DEFAULT | FPSCR_LEN(3) | FPSCR_STRIDE(3));
    check(p != NULL, "overlength process");
    if (!p) return;
    SnxArmCPU* cpu = &p->cpu.state.cpu;
    cpu->d[6] = 0x7777777788888888ull;
    SnxArmBlock block = compile(exec, words, 2, SNX_ARM_PROFILE_ARM11, true);
    if (compiled(block, "overlength")) {
        check(run(p, block) == SNX_CTR_RUN_UNSUPPORTED, "an overlength short vector fails closed");
        check(cpu->d[6] == 0x7777777788888888ull, "an overlength short vector writes nothing");
    }
    snx_ctr_process_free(p);
}

/* A second source in the first bank is a scalar broadcast: every lane adds
   S0, and the source bank keeps its lanes. */
static void broadcast(SnxArmExec* exec) {
    static const uint32_t words[] = {0xee384a00u, 0xeafffffeu}; /* VADD.F32 S8,S16,S0 */
    SnxCtrProcess* p = make_process(words, 2, FPSCR_DEFAULT | FPSCR_LEN(3));
    check(p != NULL, "broadcast process");
    if (!p) return;
    SnxArmCPU* cpu = &p->cpu.state.cpu;
    cpu->d[0] = 0x1111111140000000ull; /* S0 = 2.0, S1 sentinel */
    cpu->d[4] = 0x1111111122222222ull;
    cpu->d[5] = 0x3333333344444444ull;
    cpu->d[8] = 0x400000003f800000ull; /* S16 = 1.0, S17 = 2.0 */
    cpu->d[9] = 0x4080000040400000ull; /* S18 = 3.0, S19 = 4.0 */
    SnxArmBlock block = compile(exec, words, 2, SNX_ARM_PROFILE_ARM11, true);
    if (compiled(block, "broadcast")) {
        check(run(p, block) == SNX_CTR_RUN_BUDGET, "the M broadcast runs");
        check(cpu->d[4] == 0x4080000040400000ull, "broadcast S8 = 3.0 and S9 = 4.0");
        check(cpu->d[5] == 0x40c0000040a00000ull, "broadcast S10 = 5.0 and S11 = 6.0");
        check(cpu->d[0] == 0x1111111140000000ull, "broadcast leaves the scalar source alone");
        check(cpu->d[8] == 0x400000003f800000ull && cpu->d[9] == 0x4080000040400000ull,
              "broadcast leaves the vector source alone");
    }
    snx_ctr_process_free(p);
}

/* A first source in the first bank (S0-S7) is not modelled: the word fails
   closed and the destination keeps its lanes. */
static void n_bank0(SnxArmExec* exec) {
    static const uint32_t words[] = {0xee304a0au, 0xeafffffeu}; /* VADD.F32 S8,S0,S20 */
    SnxCtrProcess* p = make_process(words, 2, FPSCR_DEFAULT | FPSCR_LEN(3));
    check(p != NULL, "N-bank0 process");
    if (!p) return;
    SnxArmCPU* cpu = &p->cpu.state.cpu;
    cpu->d[0] = 0x111111113f800000ull;
    cpu->d[4] = 0x1111111122222222ull;
    cpu->d[10] = 0x0000000040400000ull;
    SnxArmBlock block = compile(exec, words, 2, SNX_ARM_PROFILE_ARM11, true);
    if (compiled(block, "N-bank0")) {
        check(run(p, block) == SNX_CTR_RUN_UNSUPPORTED, "a first-bank first source stays unsupported");
        check(cpu->d[4] == 0x1111111122222222ull, "the unsupported first source writes nothing");
    }
    snx_ctr_process_free(p);
}

/* VCMP.F32 S0,S0, LEN=1: compares are scalar whatever LEN says. This backend
   does not model them, so the valid scalar-only operation stays unsupported. */
static void scalar_only(SnxArmExec* exec) {
    static const uint32_t words[] = {0xeeb40a60u, 0xeafffffeu};
    SnxCtrProcess* p = make_process(words, 2, FPSCR_DEFAULT | FPSCR_LEN(1));
    check(p != NULL, "compare process");
    if (!p) return;
    SnxArmCPU* cpu = &p->cpu.state.cpu;
    cpu->d[0] = 0x400000003f800000ull;
    uint32_t fpscr = cpu->fpscr;
    SnxArmBlock block = compile(exec, words, 2, SNX_ARM_PROFILE_ARM11, true);
    if (compiled(block, "compare")) {
        check(run(p, block) == SNX_CTR_RUN_UNSUPPORTED, "an unmodelled scalar-only compare stays unsupported");
        check(cpu->d[0] == 0x400000003f800000ull && cpu->fpscr == fpscr, "the compare left the state alone");
    }
    snx_ctr_process_free(p);
}

/* An enabled FP exception is not modelled; the backend must fail closed. */
static void enabled_exception(SnxArmExec* exec) {
    static const uint32_t words[] = {0xee300a81u, 0xeafffffeu};
    SnxCtrProcess* p = make_process(words, 2, FPSCR_DEFAULT | FPSCR_IDE);
    check(p != NULL, "exception process");
    if (!p) return;
    SnxArmCPU* cpu = &p->cpu.state.cpu;
    cpu->d[0] = 0x3f80000000000000ull;
    SnxArmBlock block = compile(exec, words, 2, SNX_ARM_PROFILE_ARM11, true);
    if (compiled(block, "exception")) {
        check(run(p, block) == SNX_CTR_RUN_UNSUPPORTED, "an enabled exception is not executed");
        check(cpu->d[0] == 0x3f80000000000000ull, "no lane written under an enabled exception");
    }
    snx_ctr_process_free(p);
}

/* D16..D31 are rejected by the ARM11 profile; the callback must not revive a
   word the block stopped as SNX_ARM_UNDEFINED. */
static void profile_rejected(SnxArmExec* exec) {
    static const uint32_t words[] = {0xeec20b02u, 0xeafffffeu}; /* VDIV.F64 D16,D2,D2 */
    SnxCtrProcess* p = make_process(words, 2, FPSCR_DEFAULT | FPSCR_LEN(3));
    check(p != NULL, "profile process");
    if (!p) return;
    SnxArmCPU* cpu = &p->cpu.state.cpu;
    cpu->d[16] = 0x123456789abcdef0ull;
    SnxArmBlock block = compile(exec, words, 2, SNX_ARM_PROFILE_ARM11, true);
    if (compiled(block, "profile")) {
        check(run(p, block) == SNX_CTR_RUN_FAULT, "D16..D31 stay rejected");
        check(cpu->d[16] == 0x123456789abcdef0ull, "profile-rejected destination untouched");
    }
    snx_ctr_process_free(p);
}

/* VMSR writes LEN/STRIDE on the ARM11; the following VADD then runs as a
   short vector. */
static void vmsr(SnxArmExec* exec) {
    static const uint32_t words[] = {0xeee10a10u, 0xee384a0au, 0xeafffffeu}; /* VMSR FPSCR, r0 */
    SnxCtrProcess* p = make_process(words, 3, FPSCR_DEFAULT);
    check(p != NULL, "vmsr process");
    if (!p) return;
    SnxArmCPU* cpu = &p->cpu.state.cpu;
    cpu->r[0] = FPSCR_DEFAULT | FPSCR_LEN(3);
    load_f32_sources(cpu->d);
    SnxArmBlock block = compile(exec, words, 3, SNX_ARM_PROFILE_ARM11, true);
    if (compiled(block, "vmsr")) {
        check(run(p, block) == SNX_CTR_RUN_BUDGET, "VMSR enables the short vector");
        check(cpu->fpscr == (FPSCR_DEFAULT | FPSCR_LEN(3)), "FPSCR keeps LEN/STRIDE");
        check(cpu->d[4] == 0x41b0000041300000ull, "VMSR path S8..S9 = 11,22");
        check(cpu->d[5] == 0x4230000042040000ull, "VMSR path S10..S11 = 33,44");
    }
    snx_ctr_process_free(p);
}

/* The ARM11 require is (value & ~0xf3f7009f) == 0: it excludes QC (bit 27)
   and AHP (bit 26), so those VMSR values trap before the wider PUT mask can
   store them. The exception enables fail closed at the same require. */
static void vmsr_mask(SnxArmExec* exec) {
    static const uint32_t words[] = {0xeee10a10u, 0xeafffffeu};
    static const char* const cases[] = {"VMSR QC stays outside the ARM11 require",
                                        "VMSR AHP stays outside the ARM11 require",
                                        "VMSR exception enable stays outside the ARM11 require"};
    static const uint32_t values[] = {FPSCR_DEFAULT | FPSCR_QC, FPSCR_DEFAULT | FPSCR_AHP, FPSCR_DEFAULT | FPSCR_IDE};
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        SnxCtrProcess* p = make_process(words, 2, FPSCR_DEFAULT);
        check(p != NULL, "mask-control process");
        if (!p) continue;
        SnxArmCPU* cpu = &p->cpu.state.cpu;
        cpu->r[0] = values[i];
        uint32_t fpscr = cpu->fpscr;
        SnxArmBlock block = compile(exec, words, 2, SNX_ARM_PROFILE_ARM11, true);
        if (compiled(block, cases[i])) {
            check(run(p, block) == SNX_CTR_RUN_UNSUPPORTED, cases[i]);
            check(cpu->fpscr == fpscr && cpu->r[0] == values[i], "the rejected VMSR left FPSCR and r0 alone");
        }
        snx_ctr_process_free(p);
    }
}

/* The wider VMSR mask is ARM11-only: a flat A57 block stores the same value
   with the narrow mask, dropping LEN/STRIDE, and VMRS reads it back. */
static void a57_vmsr(SnxArmExec* exec) {
    static const uint32_t words[] = {0xeee10a10u, 0xeef10a10u, 0xeafffffeu}; /* VMSR; VMRS; B . */
    SnxCtrProcess* p = make_process(words, 3, FPSCR_DEFAULT);
    check(p != NULL, "A57 process");
    if (!p) return;
    SnxArmCPU* cpu = &p->cpu.state.cpu;
    cpu->r[0] = FPSCR_DEFAULT | FPSCR_LEN(3);
    SnxArmBlock block = compile(exec, words, 3, SNX_ARM_PROFILE_A57, false);
    if (compiled(block, "A57 VMSR/VMRS")) {
        check(run(p, block) == SNX_CTR_RUN_BUDGET, "the A57 VMSR/VMRS block runs");
        check(cpu->fpscr == FPSCR_DEFAULT, "A57 drops LEN/STRIDE from FPSCR");
        check(cpu->r[0] == FPSCR_DEFAULT, "VMRS reads the A57 FPSCR");
    }
    snx_ctr_process_free(p);
}

/* A thread created by SVC runs with its own SnxArmCPU context; the backend
   must be inherited from the thread that created it. */
static void new_thread(SnxArmExec* exec) {
    static const uint32_t words[] = {0xee384a0au, 0xeafffffeu};
    SnxCtrProcess* p = make_process(words, 2, FPSCR_DEFAULT);
    check(p != NULL, "thread process");
    if (!p) return;
    uint32_t* r = p->cpu.state.cpu.r;
    r[0] = 0x20;
    r[1] = 0x100000;
    r[2] = 0;
    r[3] = SNX_CTR_STACK_TOP - 0x1000;
    r[4] = 0xfffffffeu;
    check(snx_ctr_svc(p, 8) == SNX_CTR_SVC_RETURN && !r[0], "thread created");
    uint32_t tid = snx_ctr_handle_object(p, r[1]);
    check(tid > 2u, "thread handle");
    if (tid <= 2u) {
        snx_ctr_process_free(p);
        return;
    }
    CtrThread* t = &p->objects[tid].data.thread;
    t->context.fpscr = FPSCR_DEFAULT | FPSCR_LEN(3);
    load_f32_sources(t->context.d);
    check(snx_ctr_schedule(p) && p->current_thread == tid, "the new thread runs");
    SnxArmBlock block = compile(exec, words, 2, SNX_ARM_PROFILE_ARM11, true);
    if (compiled(block, "thread")) {
        check(run(p, block) == SNX_CTR_RUN_BUDGET, "the new thread runs the short vector");
        check(p->cpu.state.cpu.d[4] == 0x41b0000041300000ull, "thread S8..S9 = 11,22");
        check(p->cpu.state.cpu.d[5] == 0x4230000042040000ull, "thread S10..S11 = 33,44");
    }
    snx_ctr_process_free(p);
}

int main(void) {
    char error[512] = {0};
    SnxArmExec* exec = snx_arm_exec_new(error, sizeof error);
    if (!exec) {
        printf("FAIL: snx_arm_exec_new: %s\n", error);
        return 1;
    }
    scalar_block(exec);
    vector_f32(exec);
    vector_mul(exec);
    vector_f64(exec);
    bank_zero(exec);
    scalar_len0(exec);
    stride2_f32(exec);
    stride2_f64(exec);
    invalid_stride(exec);
    overlength(exec);
    broadcast(exec);
    n_bank0(exec);
    scalar_only(exec);
    enabled_exception(exec);
    profile_rejected(exec);
    vmsr(exec);
    vmsr_mask(exec);
    a57_vmsr(exec);
    new_thread(exec);
    snx_arm_exec_free(exec);
    if (failures) return 1;
    puts("ARM11 VFP short vectors, bank rotation, scalar operations and the thread backend passed");
    return 0;
}
