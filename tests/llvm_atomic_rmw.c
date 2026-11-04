/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The LSE atomics' read-modify-write as each target gets it (ir_ldst.c's atomic_rmw): an atomicrmw where the target's
   LLVM lowers it right, else a compare-and-swap loop. PowerPC64's byte and halfword UMAX and UMIN are the loop (its
   LLVM compares the operand in a whole 32-bit register after lbarx or lharx, the bits above it never cleared), its
   word and doubleword ones and the signed ones are the atomicrmw, and so are x86-64's of every size; big-endian
   PowerPC64 loops for every arithmetic one, as its values are the guest's bytes reversed. */
#include "common/utilities/common.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include <llvm-c/Target.h>
#include <stdio.h>
#include <string.h>

enum { OPC_SMAX = 4, OPC_UMAX = 6, OPC_UMIN = 7 };

/* LDSMAX, LDUMAX or LDUMIN of 1 << size bytes: Rs w2, Rn x0, Rt w1 */
static SnxU32 word_of(unsigned size, unsigned opc) {
    return (SnxU32)size << 30 | 0x38200000u | 2u << 16 | opc << 12 | 0u << 5 | 1u;
}

/* the function's atomicrmw of op (counted in rmw) and cmpxchg instructions */
static void count(LLVMValueRef fn, LLVMAtomicRMWBinOp op, unsigned* rmw, unsigned* cas) {
    *rmw = *cas = 0;
    for (LLVMBasicBlockRef b = LLVMGetFirstBasicBlock(fn); b; b = LLVMGetNextBasicBlock(b))
        for (LLVMValueRef i = LLVMGetFirstInstruction(b); i; i = LLVMGetNextInstruction(i)) {
            if (LLVMGetInstructionOpcode(i) == LLVMAtomicRMW && LLVMGetAtomicRMWBinOp(i) == op) ++*rmw;
            if (LLVMGetInstructionOpcode(i) == LLVMAtomicCmpXchg) ++*cas;
        }
}

int main(void) {
    static const char* const triples[] = {"x86_64-unknown-linux-gnu", "powerpc64le-unknown-linux-gnu",
                                          "powerpc64-unknown-linux-gnu"};
    static const char* const names[] = {"SMAX", "", "UMAX", "UMIN"};
    char error[1024] = {0};
    int failed = 0, ran = 0;
    if (snx_ir_init(error, sizeof error)) return snx_error("%s", error);
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    for (unsigned t = 0; t < sizeof(triples) / sizeof(triples[0]); ++t) {
        SnxIr* ir = snx_ir_new_target("atomics", '2', triples[t], error, sizeof error);
        if (!ir) {
            printf("%s not checked: %s\n", triples[t], error);
            continue;
        }
        ++ran;
        for (unsigned size = 0; size < 4; ++size)
            for (unsigned opc = OPC_SMAX; opc <= OPC_UMIN; ++opc) {
                const LLVMAtomicRMWBinOp op = opc == OPC_SMAX   ? LLVMAtomicRMWBinOpMax
                                              : opc == OPC_UMAX ? LLVMAtomicRMWBinOpUMax
                                                                : LLVMAtomicRMWBinOpUMin;
                const int loop = ir->big_endian || (ir->ppc && size < 2 && opc != OPC_SMAX);
                char name[32];
                unsigned rmw, cas;
                if (opc == 5) continue; /* LDSMIN: as LDSMAX */
                snprintf(name, sizeof name, "S_%u_%u", size, opc);
                LLVMValueRef fn = snx_ir_static_step(ir, word_of(size, opc), 0x1000, SNX_A64_TARGET_MAX, name);
                if (!fn || ir->fallbacks) {
                    fprintf(stderr, "%s: LD%s of %u bytes (%08x) has no emitter\n", triples[t], names[opc - OPC_SMAX],
                            1u << size, (unsigned)word_of(size, opc));
                    failed = 1;
                    ir->fallbacks = 0;
                    continue;
                }
                count(fn, op, &rmw, &cas);
                if (loop ? rmw || !cas : !rmw || cas) {
                    fprintf(stderr,
                            "%s: LD%s of %u bytes (%08x) has %u atomicrmw and %u cmpxchg, where it should be %s\n",
                            triples[t], names[opc - OPC_SMAX], 1u << size, (unsigned)word_of(size, opc), rmw, cas,
                            loop ? "a compare-and-swap loop" : "one atomicrmw");
                    failed = 1;
                }
            }
        snx_ir_free(ir);
    }
    if (!ran) {
        printf("llvm_atomic_rmw skipped: this LLVM has none of the targets\n");
        return 77;
    }
    if (!failed) printf("LDSMAX, LDUMAX and LDUMIN of 1 to 8 bytes as each target should have them\n");
    return failed;
}
