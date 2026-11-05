/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise promotion's incoming-edge index with clean/dirty joins, critical edges and duplicate switch edges.
   --bench N [OUT.ll] measures promotion alone. Its checksum includes all emitted IR and synchronization counts. */
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/aarch64/cpu.h"
#include "common/utilities/bits.h"
#include <llvm-c/Analysis.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    double elapsed;
    u64 hash, edge_stores, store_bytes;
} Measurement;

static int run(unsigned count, const char* dump, Measurement* result) {
    char error[1024];
    if (snx_ir_init(error, sizeof error)) return snx_error("%s", error);
    SnxIr* ir = snx_ir_new("edges", '2', error, sizeof error);
    if (!ir) return snx_error("%s", error);
    ir->stats.enabled = 1;
    LLVMTypeRef type = LLVMFunctionType(ir->t_void, &ir->t_ptr, 1, 0);
    LLVMValueRef fn = LLVMAddFunction(ir->mod, "edges", type);
    LLVMValueRef probe = LLVMAddFunction(ir->mod, "edge_probe", type);
    snx_ir_begin(ir, fn);
    for (unsigned i = 0; i < count; ++i) {
        LLVMBasicBlockRef dirty = ir_block(ir, "dirty"), clean = ir_block(ir, "clean");
        LLVMBasicBlockRef bypass = ir_block(ir, "bypass"), join = ir_block(ir, "join");
        LLVMValueRef selector = ir_x(ir, 1);
        LLVMBuildCondBr(ir->b, LLVMBuildICmp(ir->b, LLVMIntNE, selector, ir_i64(ir, 0), ""), dirty, clean);
        LLVMPositionBuilderAtEnd(ir->b, dirty);
        for (unsigned k = 0; k < 4; ++k) {
            LLVMValueRef value = ir_cpu_load(ir, ir->t_i64, ir_off_x(0));
            ir_cpu_store(ir, LLVMBuildAdd(ir->b, value, ir_i64(ir, i + k + 1), ""), ir_off_x(0));
        }
        if (i % 3 == 0) LLVMBuildBr(ir->b, join);
        else if (i % 3 == 1)
            LLVMBuildCondBr(ir->b, LLVMBuildICmp(ir->b, LLVMIntEQ, selector, ir_i64(ir, 1), ""), join, bypass);
        else {
            LLVMValueRef branch = LLVMBuildSwitch(ir->b, selector, bypass, 2);
            LLVMAddCase(branch, ir_i64(ir, 1), join);
            LLVMAddCase(branch, ir_i64(ir, 2), join);
        }
        LLVMPositionBuilderAtEnd(ir->b, clean);
        LLVMBuildBr(ir->b, join);
        LLVMPositionBuilderAtEnd(ir->b, bypass);
        LLVMBuildBr(ir->b, join);
        LLVMPositionBuilderAtEnd(ir->b, join);
        LLVMBuildCall2(ir->b, type, probe, &ir->c, 1, "");
    }
    LLVMBuildRetVoid(ir->b);
    double start = snx_seconds();
    ir_promote_registers(ir, fn);
    double elapsed = snx_seconds() - start;
    ir_check_promoted(ir, fn);
    char* message = NULL;
    int failed = LLVMVerifyModule(ir->mod, LLVMReturnStatusAction, &message) || ir->check_failures;
    if (failed) fprintf(stderr, "Edge index: %s %s\n", message ? message : "", ir->check_why);
    LLVMDisposeMessage(message);
    char* text = snx_ir_text(ir);
    u64 hash = snx_fnv1a(SNX_FNV_BASIS, text, strlen(text));
    if (dump && snx_write_file(dump, text, strlen(text))) failed = 1;
    if (!ir->stats.edge_stores || !ir->stats.promoted_slots) failed = 1;
    *result = (Measurement){elapsed, hash, ir->stats.edge_stores, ir->stats.store_bytes};
    LLVMDisposeMessage(text);
    snx_ir_free(ir);
    return failed;
}
int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "--bench")) {
        char* end;
        unsigned long count = strtoul(argv[2], &end, 10);
        if (*end || !count || count > 8192) return 2;
        unsigned long repeat = 1;
        const char* dump = argc > 3 ? argv[3] : NULL;
        if (dump && !strcmp(dump, "--repeat")) {
            if (argc != 5) return 2;
            repeat = strtoul(argv[4], &end, 10);
            if (*end || !repeat || repeat > 4096) return 2;
            dump = NULL;
        }
        double elapsed = 0;
        Measurement first = {0};
        for (unsigned long i = 0; i < repeat; ++i) {
            Measurement result;
            if (run((unsigned)count, dump, &result)) return 1;
            if (!i) first = result;
            if (result.hash != first.hash || result.edge_stores != first.edge_stores ||
                result.store_bytes != first.store_bytes) return 1;
            elapsed += result.elapsed;
        }
        printf("{\"elapsed_ns\":%.0f,\"checksum\":\"%016llx-%llu-%llu\"}\n", elapsed * 1e9,
            (unsigned long long)first.hash, (unsigned long long)first.edge_stores,
            (unsigned long long)first.store_bytes);
        return 0;
    }
    Measurement result;
    return run(32, NULL, &result) || run(1024, NULL, &result);
}
