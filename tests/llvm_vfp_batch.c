/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Whole-vector half conversions after ordinary, DUP and zero producers: no helper calls and exact full state. */
#include "llvm_steps.h"
#include "common/cpu/recompiler/codegen.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <string.h>

static const u32 conversions[] = {0x0e217820u, 0x4e217820u, 0x0e216820u, 0x4e216820u};
static const u32 edges[] = {0, 1, 0x8000, 0x3ff, 0x400, 0x3555, 0x7c00, 0x7d01, 0x7e02, 0xffff,
                            0x80000000, 0x007fffff, 0x00800000, 0x3fc00000, 0x7f800000, 0x7f800001,
                            0x7fc12345, 0xffffffff};
static unsigned convert_calls(LLVMValueRef function, const char* name) {
    unsigned count = 0;
    for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(function); block; block = LLVMGetNextBasicBlock(block))
        for (LLVMValueRef inst = LLVMGetFirstInstruction(block); inst; inst = LLVMGetNextInstruction(inst)) {
            if (!LLVMIsACallInst(inst)) continue;
            LLVMValueRef callee = LLVMGetCalledValue(inst);
            size_t length = 0;
            const char* called = LLVMGetValueName2(callee, &length);
            if (length == strlen(name) && !memcmp(called, name, length)) count++;
        }
    return count;
}
static void reference(CPU* c, unsigned index, int broadcast) {
    unsigned from = index < 2 ? 16 : 32, to = index < 2 ? 32 : 16;
    if (broadcast == 2) c->v[1] = (V128){0};
    else if (broadcast) {
        for (unsigned lane = 0; lane < 128 / from; lane++) {
            if (from == 16) SNX_VSET_H(c->v[1], lane, c->x[1]);
            else SNX_VSET_S(c->v[1], lane, c->x[1]);
        }
    }
    V128 source = c->v[1];
    if (index != 3) c->v[0] = (V128){0};
    for (unsigned lane = 0; lane < 4; lane++) {
        unsigned at = lane + (index == 1 ? 4 : 0);
        u64 bits = from == 16 ? SNX_VH(source, at) : SNX_VS(source, at);
        SnxFpReply reply = snx_fp_convert(bits, (int)from, (int)to, (int)(c->fpcr >> 22 & 3), c->fpcr);
        c->fpsr |= reply.flags;
        if (to == 16) SNX_VSET_H(c->v[0], lane + (index == 3 ? 4 : 0), reply.value);
        else SNX_VSET_S(c->v[0], lane, reply.value);
    }
    c->pc = c->x[30];
}
static int run(int jit, char opt) {
    LlvmSteps steps;
    char error[1024];
    if (llvm_steps_begin_opt(&steps, jit, LLVM_STEPS_PORTABLE, opt, error, sizeof error)) return snx_error("%s", error);
    u8 bytes[12][12], leaders[12][3] = {{0}};
    SnxCodeImage images[12] = {{0}};
    SnxFunction functions[12] = {{0}};
    SnxCodegen code = {.modules = images, .module_count = 12, .functions = functions, .function_count = 12};
    for (unsigned i = 0; i < 12; i++) {
        u64 base = 0x1000 + i * 0x100;
        unsigned index = i % 4;
        snx_st_le32(bytes[i], i < 4 ? 0xd503201fu : i >= 8 ? 0x4f000401u : index < 2 ? 0x4e020c21u : 0x4e040c21u);
        snx_st_le32(bytes[i] + 4, conversions[index]);
        snx_st_le32(bytes[i] + 8, 0xd65f03c0u);
        leaders[i][0] = 1;
        images[i] = (SnxCodeImage){.image = bytes[i], .image_size = 12, .base = base, .text_size = 12};
        functions[i] = (SnxFunction){.start = base, .end = base + 12, .count = 3,
                                    .leaders = leaders[i], .leader_count = 1};
    }
    for (unsigned i = 0; i < 12; i++) llvm_steps_add_function(&steps, &code, &functions[i]);
    if (opt != '0' && !steps.ir->big_endian) {
        if (snx_ir_module_optimize(steps.ir, steps.ir->mod, opt, error, sizeof error)) return snx_error("%s", error);
        for (unsigned i = 0; i < 12; i++)
            if (convert_calls(steps.fns[i], "snx_fp_convert") || convert_calls(steps.fns[i], "snx_fp_host_lanes"))
                return snx_error("half conversion sequence %u still calls a helper at O%c", i, opt);
    }
    if (llvm_steps_load(&steps, error, sizeof error)) return snx_error("%s", error);
    for (unsigned i = 0; i < 12; i++) {
        GuestFn function = (GuestFn)(size_t)steps.steps[i];
        for (unsigned pattern = 0; pattern < sizeof edges / sizeof *edges; pattern++)
            for (unsigned mode = 0; mode < 64; mode++) {
                CPU actual = {0};
                actual.pc = functions[i].start;
                actual.fpcr = (mode & 3) << 22 | (mode >> 2 & 1) << 24 | (mode >> 3 & 1) << 25 | (mode >> 4 & 1) << 26 | (mode >> 5 & 1) << 19;
                actual.fpsr = pattern & 1 ? 0x0800009fu : 0;
                actual.x[1] = edges[pattern];
                for (unsigned lane = 0; lane < 4; lane++) {
                    SNX_VSET_S(actual.v[0], lane, 0xfedcba98u + lane);
                    SNX_VSET_S(actual.v[1], lane, edges[(pattern + lane) % (sizeof edges / sizeof *edges)]);
                }
                CPU expected = actual;
                reference(&expected, i % 4, (int)(i / 4));
                function(&actual, functions[i].start);
                if (memcmp(&actual, &expected, sizeof actual))
                    return snx_error("conversion sequence %u pattern %u mode %u O%c jit%d", i, pattern, mode, opt, jit);
            }
    }
    llvm_steps_end(&steps);
    return 0;
}
int main(void) {
    for (int jit = 0; jit < 2; jit++)
        for (unsigned opt = 0; opt < 3; opt++)
            if (run(jit, (char)('0' + opt))) return 1;
    return 0;
}
