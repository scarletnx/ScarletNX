/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The FP fast paths (ir_fpu.c, ir_vec_fp.c) are taken for the operands they exist for: normal operands with a normal
   result, in round to nearest, with FPCR.FZ clear, with FZ set (most games run with it), and with FZ set when FPSR's
   flags aren't kept (fpsr_flags=0). Each instruction's LLVM step (llvm_steps.c) is called from a probe function that
   hands it a CPU in a stack slot holding those constants, and the module goes through LLVM's O2: the guards fold, and
   the probe must have no call left, the soft-float helpers' included. A guard that is always false for the case
   keeps its helper call: llvm_vfp_lanes checks the results the paths give, this checks that the fast one is used.
   A probe in round towards plus infinity, which the fast paths leave to the helpers, must keep its call, so a probe
   folded away entirely can't pass, except for the instructions whose results don't depend on FPCR's rounding (the
   FRINTs and FCVTs that round in their encoding's mode, the estimates) */
#include "llvm_steps.h"
#include <llvm-c/Transforms/PassBuilder.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define FPCR_FZ   (1u << 24)
#define FPCR_RP   (1u << 22) /* RMode: towards plus infinity */
#define FPSR_IXC  (1u << 4)

typedef struct {
    const char* name;
    u32 word;
    int dbl;   /* doubles (else singles) */
    int fma;   /* needs the host's FMA for its fast path */
    int round; /* needs the host's rounding instructions (SSE4.1 on x86) */
    int modeless; /* the result doesn't depend on FPCR.RMode (an encoding's own rounding, an estimate), so round
                     towards plus infinity takes the fast path too */
} Insn;
static const Insn g_insns[] = {
    {"FADD S", 0x1E222820u, 0, 0, 0, 0},     {"FMUL S", 0x1E220820u, 0, 0, 0, 0},
    {"FDIV S", 0x1E221820u, 0, 0, 0, 0},     {"FSQRT S", 0x1E21C020u, 0, 0, 0, 0},
    {"FADD D", 0x1E622820u, 1, 0, 0, 0},     {"FMUL D", 0x1E620820u, 1, 0, 0, 0},
    {"FDIV D", 0x1E621820u, 1, 0, 0, 0},     {"FMADD S", 0x1F020C20u, 0, 1, 0, 0},
    {"FADD 4S", 0x4E22D420u, 0, 0, 0, 0},    {"FMUL 4S", 0x6E22DC20u, 0, 0, 0, 0},
    {"FDIV 4S", 0x6E22FC20u, 0, 0, 0, 0},    {"FMLA 4S", 0x4E22CC20u, 0, 1, 0, 0},
    {"FADD 2D", 0x4E62D420u, 1, 0, 0, 0},    {"FMUL 2D", 0x6E62DC20u, 1, 0, 0, 0},
    {"FDIV 2D", 0x6E62FC20u, 1, 0, 0, 0},    {"FSQRT 2D", 0x6EE1F820u, 1, 0, 0, 0},
    {"FABD S", 0x7EA2D420u, 0, 0, 0, 0},     {"FABD 4S", 0x6EA2D420u, 0, 0, 0, 0},
    {"FABD 2D", 0x6EE2D420u, 1, 0, 0, 0},    {"FRINTM S", 0x1E254020u, 0, 0, 1, 1},
    {"FRINTM D", 0x1E654020u, 1, 0, 1, 1},   {"FRINTX S", 0x1E274020u, 0, 0, 1, 0},
    {"FRINTM 4S", 0x4E219820u, 0, 0, 1, 1},  {"FRINTI 4S", 0x6EA19820u, 0, 0, 1, 0},
    {"FRINTX 2D", 0x6E619820u, 1, 0, 1, 0},  {"FRINTA S", 0x1E264020u, 0, 0, 1, 1},
    {"FRINTA D", 0x1E664020u, 1, 0, 1, 1},   {"FRINTA 4S", 0x6E218820u, 0, 0, 1, 1},
    {"FRINTA 2D", 0x6E618820u, 1, 0, 1, 1},  {"FCVTAS W S", 0x1E240020u, 0, 0, 1, 1},
    {"FCVTAU 4S", 0x6E21C820u, 0, 0, 1, 1},  {"FRECPE S", 0x5EA1D820u, 0, 0, 0, 1},
    {"FRECPE D", 0x5EE1D820u, 1, 0, 0, 1},   {"FRSQRTE S", 0x7EA1D820u, 0, 0, 0, 1},
    {"FRSQRTE D", 0x7EE1D820u, 1, 0, 0, 1},  {"FRECPE 4S", 0x4EA1D820u, 0, 0, 0, 1},
    {"FRSQRTE 2D", 0x6EE1D820u, 1, 0, 0, 1},
};
#define NINSNS (sizeof(g_insns) / sizeof(g_insns[0]))

typedef struct {
    const char* name;
    u32 fpcr;
    int fpsr_flags; /* snx_ir_fpsr_flags while the steps are made */
    int fast;       /* the probe must have no call left (else it must keep one) */
} Case;
static const Case g_cases[] = {
    {"FZ clear", 0, 1, 1},
    {"FZ set", FPCR_FZ, 1, 1},
    {"FZ set, fpsr_flags=0", FPCR_FZ, 0, 1},
    {"round towards plus infinity (the helpers)", FPCR_RP, 1, 0},
};
#define NCASES (sizeof(g_cases) / sizeof(g_cases[0]))

static LLVMValueRef offset(SnxIr* ir, LLVMValueRef base, size_t at) {
    LLVMValueRef index = LLVMConstInt(ir->t_i64, at, 0);
    return LLVMBuildGEP2(ir->b, ir->t_i8, base, &index, 1, "");
}
static void put(SnxIr* ir, LLVMValueRef base, size_t at, LLVMTypeRef type, unsigned long long value) {
    LLVMBuildStore(ir->b, LLVMConstInt(type, value, 0), offset(ir, base, at));
}

/* probe(): a zeroed CPU with fpcr, IXC set (the FMA paths want it, and an inexact quotient changes nothing then)
   and 1.5 and 2.25 in every lane of V1 and V2 (2.25 in V3, FMADD's addend), the step called on it, V0's low half
   returned */
static LLVMValueRef probe(SnxIr* ir, LLVMValueRef step, const Insn* insn, u32 fpcr, const char* name) {
    LLVMValueRef fn = LLVMAddFunction(ir->mod, name, LLVMFunctionType(ir->t_i64, NULL, 0, 0)), c, arg;
    unsigned long long a = insn->dbl ? 0x3FF8000000000000ull : 0x3FC000003FC00000ull;
    unsigned long long b = insn->dbl ? 0x4002000000000000ull : 0x4010000040100000ull;
    unsigned v, half;
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, fn, "entry"));
    c = LLVMBuildAlloca(ir->b, LLVMArrayType(ir->t_i8, (unsigned)sizeof(CPU)), "c");
    LLVMSetAlignment(c, 16);
    LLVMBuildMemSet(ir->b, c, LLVMConstInt(ir->t_i8, 0, 0), LLVMConstInt(ir->t_i64, sizeof(CPU), 0), 16);
    put(ir, c, offsetof(CPU, fpcr), ir->t_i32, fpcr);
    put(ir, c, offsetof(CPU, fpsr), ir->t_i32, FPSR_IXC);
    for (v = 1; v <= 3; ++v) { /* whole registers, which LLVM forwards to the step's loads of whole vectors */
        LLVMValueRef halves[2];
        for (half = 0; half < 2; ++half) halves[half] = LLVMConstInt(ir->t_i64, v == 1 ? a : b, 0);
        LLVMBuildStore(ir->b, LLVMConstVector(halves, 2), offset(ir, c, offsetof(CPU, v) + v * sizeof(V128)));
    }
    arg = c;
    LLVMBuildCall2(ir->b, LLVMGlobalGetValueType(step), step, &arg, 1, "");
    LLVMBuildRet(ir->b, LLVMBuildLoad2(ir->b, ir->t_i64, offset(ir, c, offsetof(CPU, v)), "r"));
    return fn;
}

/* the calls left in fn that aren't LLVM's intrinsics */
static unsigned calls(LLVMValueRef fn, char* first, size_t first_size) {
    LLVMBasicBlockRef block;
    LLVMValueRef inst;
    unsigned n = 0;
    for (block = LLVMGetFirstBasicBlock(fn); block; block = LLVMGetNextBasicBlock(block))
        for (inst = LLVMGetFirstInstruction(block); inst; inst = LLVMGetNextInstruction(inst)) {
            LLVMValueRef callee;
            size_t length = 0;
            const char* name = "an indirect call";
            if (LLVMGetInstructionOpcode(inst) != LLVMCall) continue;
            callee = LLVMGetCalledValue(inst);
            if (callee && LLVMIsAFunction(callee)) {
                name = LLVMGetValueName2(callee, &length);
                if (LLVMGetIntrinsicID(callee)) continue;
            }
            if (!n++) snprintf(first, first_size, "%s", name);
        }
    return n;
}

int main(void) {
    char error[1024], name[64], first[128];
    LlvmSteps s;
    LLVMValueRef probes[NCASES][NINSNS];
    LLVMPassBuilderOptionsRef options;
    LLVMErrorRef failed;
    unsigned k, i, checked = 0, failures = 0;
    int fma, round;
    if (llvm_steps_begin(&s, 0, 0, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    fma = (s.ir->features->x86 && s.ir->features->fma) || s.ir->features->aarch64;
    round = (s.ir->features->x86 && s.ir->features->sse42) || s.ir->features->aarch64;
    for (k = 0; k < NCASES; ++k) {
        snx_ir_fpsr_flags(g_cases[k].fpsr_flags);
        for (i = 0; i < NINSNS; ++i) {
            size_t at;
            probes[k][i] = NULL;
            if ((g_insns[i].fma && !fma) || (g_insns[i].round && !round)) continue;
            at = llvm_steps_add(&s, g_insns[i].word, 0x1000, 0);
            LLVMAddAttributeAtIndex(s.fns[at], LLVMAttributeFunctionIndex,
                                    LLVMCreateEnumAttribute(s.ir->ctx, LLVMGetEnumAttributeKindForName("alwaysinline", 12), 0));
            snprintf(name, sizeof(name), "probe_%u_%u", k, i);
            probes[k][i] = probe(s.ir, s.fns[at], &g_insns[i], g_cases[k].fpcr, name);
        }
    }
    snx_ir_fpsr_flags(1);
    options = LLVMCreatePassBuilderOptions();
    failed = LLVMRunPasses(s.ir->mod, "default<O2>", s.ir->tm, options);
    LLVMDisposePassBuilderOptions(options);
    if (failed) {
        char* message = LLVMGetErrorMessage(failed);
        fprintf(stderr, "LLVM passes: %s\n", message);
        LLVMDisposeErrorMessage(message);
        return 1;
    }
    for (k = 0; k < NCASES; ++k)
        for (i = 0; i < NINSNS; ++i) {
            unsigned n;
            int fast = g_cases[k].fast || g_insns[i].modeless;
            if (!probes[k][i]) continue;
            first[0] = 0;
            n = calls(probes[k][i], first, sizeof(first));
            ++checked;
            if (fast ? n != 0 : n == 0) {
                ++failures;
                if (fast)
                    fprintf(stderr, "FAIL: %s, %s: the fast path isn't taken (%u call%s left, to %s)\n", g_insns[i].name,
                            g_cases[k].name, n, n == 1 ? "" : "s", first);
                else
                    fprintf(stderr, "FAIL: %s, %s: no helper call is left\n", g_insns[i].name, g_cases[k].name);
            }
        }
    llvm_steps_end(&s);
    printf("FP fast paths taken: %u probes, %u failed%s\n", checked, failures, fma ? "" : " (no host FMA: FMA skipped)");
    return failures ? 1 : 0;
}
