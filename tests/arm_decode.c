/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/cpu/arm/arm.h"
#include "common/cpu/arm/simd.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    static const struct {
        uint32_t word;
        SnxA32Kind kind;
    } cases[] = {
        {0xe1a00000, SNX_A32_DATA},
        {0xe59f0004, SNX_A32_SINGLE},
        {0xe78f1000, SNX_A32_SINGLE},    /* STR r1, [pc, r0] */
        {0xe7af1000, SNX_A32_UNDEFINED}, /* STR r1, [pc, r0]!: PC base with writeback */
        {0xe1cf00d8, SNX_A32_EXTRA},
        {0xe8bd8001, SNX_A32_MULTIPLE},
        {0xe12fff3e, SNX_A32_BX},
        {0xe12fff1f, SNX_A32_BX},        /* BX pc: to the PC, in A32 */
        {0xe12fff3f, SNX_A32_UNDEFINED}, /* BLX pc: UNPREDICTABLE */
        {0xe080f211, SNX_A32_UNDEFINED}, /* ADD pc, r0, r1, LSL r2: a register-shifted register's Rd the PC */
        {0xfafffffe, SNX_A32_BLX_IMM},
        {0xe16f3f12, SNX_A32_CLZ},
        {0xe6bf3f32, SNX_A32_REV},
        {0xe6ef3072, SNX_A32_EXTEND},
        {0xe7df301f, SNX_A32_BITFIELD},
        {0xe10f3000, SNX_A32_MRS},
        {0xe128f002, SNX_A32_MSR},
        {0xee1d3f50, SNX_A32_COPROC}, /* MRC TPIDRURW */
        {0xee1d3f70, SNX_A32_COPROC}, /* MRC TPIDRURO */
        {0xee0d3f50, SNX_A32_COPROC}, /* MCR TPIDRURW */
        {0xee0d3f70, SNX_A32_COPROC}, /* MCR TPIDRURO: the backend refuses it (snx_arm_coproc_user, below) */
        {0xec510f1e, SNX_A32_COPROC}, /* MRRC p15, 1, r0, r1, c14 (CNTVCT): the backend's */
        {0xe1940f9f, SNX_A32_EXCLUSIVE},
        {0xe1841f90, SNX_A32_EXCLUSIVE},
        {0xe1b40e9f, SNX_A32_EXCLUSIVE},
        {0xe1a42e90, SNX_A32_EXCLUSIVE},
        {0xe1940c9f, SNX_A32_ACQUIRE_RELEASE},
        {0xe184fc90, SNX_A32_ACQUIRE_RELEASE},
        {0xf57ff01f, SNX_A32_CLREX},
        {0xf57ff05b, SNX_A32_BARRIER},
        {0xf5d4f008, SNX_A32_HINT},
        {0xf594f008, SNX_A32_HINT},
        {0xf4d4f008, SNX_A32_HINT},
        {0xf7d4f186, SNX_A32_HINT},
        {0xef001234, SNX_A32_SVC},
        {0xe1212374, SNX_A32_BKPT},
        /* Overlapping encoding spaces must never become an integer operation. */
        {0xee000910, SNX_A32_UNDEFINED}, /* unsupported coprocessor */
        {0xf1a00000, SNX_A32_UNDEFINED}, /* cond=1111 in the integer space is not AL */
        {0xf3a00000, SNX_A32_VECTOR},    /* VADDL.U32, the AdvSIMD space uses cond=1111 */
        {0xe14f3000, SNX_A32_UNDEFINED}, /* MRS SPSR */
        {0xe121f002, SNX_A32_MSR},       /* MSR CPSR_c: EL0 writes nothing of the c field */
        {0xe161f002, SNX_A32_UNDEFINED}, /* MSR SPSR_c */
        {0xe1b0f00e, SNX_A32_UNDEFINED}, /* MOVS PC,LR exception return */
        {0xe8fd8000, SNX_A32_UNDEFINED}, /* privileged LDM^ */
        {0xe5b44004, SNX_A32_UNDEFINED}, /* writeback/load base overlap */
        {0xe8b40010, SNX_A32_MULTIPLE},  /* LDM base in list with writeback: loaded base wins (ARM11 MPCore/Unicorn) */
        {0xe8a40010,
         SNX_A32_MULTIPLE}, /* STM base in list with writeback: stores the old base (ARM11 MPCore/Unicorn) */
        {0xe8940000, SNX_A32_UNDEFINED}, /* empty register list */
        {0xe1c410d8, SNX_A32_UNDEFINED}, /* odd doubleword register pair */
        {0xe1840f90, SNX_A32_UNDEFINED}, /* STREX status/data overlap */
        {0xe1b41f9f, SNX_A32_UNDEFINED}, /* odd exclusive pair */
        {0xe1910d9f, SNX_A32_UNDEFINED}, /* reserved exclusive operation */
        {0xe1659f1b, SNX_A32_UNDEFINED}, /* CLZ's fixed Rn field */
        {0xf57ff050, SNX_A32_UNDEFINED}, /* reserved DMB option */
        {0xf57ff06b, SNX_A32_UNDEFINED}, /* ISB must use SY */
        {0xe7bf3fd2, SNX_A32_UNDEFINED}, /* bitfield exceeds bit 31 */
        /* VFP/Advanced SIMD, verified with llvm-mc -mcpu=cortex-a57. */
        {0xee321a03, SNX_A32_VECTOR},
        {0xeeb01a43, SNX_A32_VECTOR},
        {0xeeb41ac3, SNX_A32_VECTOR},
        {0xeebd1ac3, SNX_A32_VECTOR},
        {0xeeb21a43, SNX_A32_VECTOR},
        {0xeef10a10, SNX_A32_VECTOR},
        {0xed941a03, SNX_A32_VECTOR},
        {0xecb41a04, SNX_A32_VECTOR},
        {0xf2020844, SNX_A32_VECTOR},
        {0xf3b10344, SNX_A32_VECTOR},
        {0xf2820c04, SNX_A32_VECTOR},
        {0xf3b20b06, SNX_A32_VECTOR},
        {0xf3b00304, SNX_A32_VECTOR},
        {0xf2820e04, SNX_A32_VECTOR},
        {0xfe821a03, SNX_A32_VECTOR},
        {0xfe321a03, SNX_A32_VECTOR},
        {0xee000a10, SNX_A32_VECTOR}, /* VMOV s0, r0 */
        /* Reserved SIMD/FP spaces and constraints stay undefined (verified with
           llvm-objdump on cortex-a57: these print <unknown>). */
        {0xec000b10, SNX_A32_UNDEFINED}, /* reserved VFP transfer space */
        {0xfe000a10, SNX_A32_UNDEFINED}, /* cond=1111 VFP space is not unconditional AL */
        {0xf2020845, SNX_A32_UNDEFINED}, /* three-same has a zero bit here */
        {0xf3b00c00, SNX_A32_UNDEFINED},
        {0xf3b00c10, SNX_A32_UNDEFINED},
        {0xf3b00c20, SNX_A32_UNDEFINED},
        {0xf3b00d00, SNX_A32_UNDEFINED},
        {0xf3b00e00, SNX_A32_UNDEFINED}, /* reserved VDUP lane index/size */
        /* Same spaces that Capstone and Unicorn misread: llvm-objdump disassembles
           these, so they must not be marked undefined. */
        {0xee000b10, SNX_A32_VECTOR},
        {0xf3b00001, SNX_A32_VECTOR},
        {0xf3b00140, SNX_A32_VECTOR},
        {0xecb00a01, SNX_A32_VECTOR},
        {0xf3b00704, SNX_A32_VECTOR},
    };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (snx_a32_decode(cases[i].word) != cases[i].kind) {
            fprintf(stderr, "decode %08x: got %d, expected %d\n", cases[i].word, snx_a32_decode(cases[i].word),
                    cases[i].kind);
            return 1;
        }
    }
    {
        SnxThumb t;
        /* Thumb16 high-register add is one halfword. */
        if (snx_thumb_decode(0x4440, 0, &t) != 1 || t.shape != TH_HI) return 1;
        /* Thumb32 mov immediate is two halfwords. */
        if (snx_thumb_decode(0xF04F, 0x0000, &t) != 2 || t.shape != TH_WDATA) return 1;
        /* Thumb32 wide load. */
        if (snx_thumb_decode(0xF8D1, 0x00FF, &t) != 2 || t.shape != TH_LOAD) return 1;
        /* Reserved/undefined encodings are rejected. */
        if (snx_thumb_decode(0xDE00, 0, &t) != 0) return 1; /* UDF space */
        /* B<c>.W, not wide data processing */
        if (snx_thumb_decode(0xF000, 0x83F9, &t) != 2 || t.shape != TH_BRANCH || t.op != BR_BCOND_W || t.ra != 0)
            return 1;
        /* Thumb32 instructions run as their A32 twins */
        if (snx_thumb_decode(0xE8BD, 0x8FF0, &t) != 2 || t.shape != TH_A32 || t.imm != 0xE8BD8FF0u)
            return 1; /* POP.W */
        if (snx_thumb_decode(0xE92D, 0x4FF0, &t) != 2 || t.shape != TH_A32 || t.imm != 0xE92D4FF0u)
            return 1;                                            /* PUSH.W */
        if (snx_thumb_decode(0xE8BD, 0x2010, &t) != 0) return 1; /* POP.W {r4, sp}: SP in the list */
        if (snx_thumb_decode(0xEE1D, 0x0F70, &t) != 2 || t.shape != TH_A32 || t.imm != 0xEE1D0F70u) return 1; /* MRC */
        if (snx_thumb_decode(0xED9F, 0x0B02, &t) != 2 || t.imm != 0xED9F0B02u) return 1;                      /* VLDR */
        if (snx_thumb_decode(0xEF60, 0x08E2, &t) != 2 || t.imm != 0xF26008E2u) return 1; /* VADD.I32 */
        if (snx_thumb_decode(0xF960, 0x0A8F, &t) != 2 || t.imm != 0xF4600A8Fu) return 1; /* VLD1 */
    }
    {
        /* the runtime library's coprocessor registers without a backend */
        SnxArmCPU cpu;
        memset(&cpu, 0, sizeof(cpu));
        cpu.r[3] = 0x1234;
        cpu.tpidruro = 0x5678;
        snx_arm_coproc(&cpu, 0xee0d3f50); /* MCR TPIDRURW */
        snx_arm_coproc(&cpu, 0xee1d2f70); /* MRC r2, TPIDRURO */
        if (cpu.exit || cpu.tpidrurw != 0x1234 || cpu.r[2] != 0x5678) return 1;
        snx_arm_coproc(&cpu, 0xee0d3f70); /* MCR TPIDRURO */
        if (cpu.exit != SNX_ARM_UNDEFINED || cpu.syndrome != 0xee0d3f70 || cpu.tpidruro != 0x5678) return 1;
        cpu.exit = 0;
        snx_arm_undefined(&cpu, 0xe7f000f0, SNX_ARM_UNDEFINED);
        if (cpu.exit != SNX_ARM_UNDEFINED || cpu.syndrome != 0xe7f000f0) return 1;
    }
    {
        static const uint32_t invalid[] = {0xee000910,  /* reserved coprocessor */
                                           0xec000b10,  /* reserved VFP transfer */
                                           0xfe000b10,  /* cond=1111 reserved */
                                           0xf2020845,  /* three-same zero bit */
                                           0xf3b00d00}; /* reserved VDUP lane */
        SnxArmVector vector;
        for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
            if (snx_arm_vector_decode(invalid[i], &vector)) {
                fprintf(stderr, "vector %08x decoded as %d\n", invalid[i], vector.op);
                return 1;
            }
        }
    }
    puts("A32 decode and backend stubs passed");
    return 0;
}
