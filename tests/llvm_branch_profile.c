/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The offline branch profile (common/cpu/recompiler/llvm/ir_profile.c, the setting ir_branch_profile): a branch whose
   address and instruction word a line names gets LLVM's branch weights from it, a branch with another word or address
   none; a file that cannot be read and lines that are not branch counts are said, with the value given */
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    int failures;
    char said[4096]; /* snx_error's lines */
} g_test;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\nsaid:\n%s", __FILE__, __LINE__, #x, g_test.said);                              \
            ++g_test.failures;                                                                                         \
        }                                                                                                              \
    } while (0)

static void said(const char* line) {
    size_t n = strlen(g_test.said);
    snprintf(g_test.said + n, sizeof g_test.said - n, "%s\n", line);
}

/* a module of three functions, each a conditional branch the profile is asked about: at 0x1000 with the word the
   profile names, at 0x1000 with another word, at 0x3000. How many got weights; the IR in text when one did */
static int weighted(char* ir_text, size_t size) {
    static const SnxU64 pcs[] = {0x1000, 0x1000, 0x3000};
    static const SnxU32 words[] = {0xD65F03C0, 0xD65F03C1, 0xD65F03C0};
    char error[256];
    int count = 0;
    SnxIr* ir = snx_ir_new("profile", '2', error, sizeof error);
    CHECK(ir);
    if (!ir) return -1;
    for (int i = 0; i < 3; i++) {
        char name[16];
        snprintf(name, sizeof name, "fn_%d", i);
        LLVMValueRef fn = LLVMAddFunction(ir->mod, name, LLVMFunctionType(ir->t_void, NULL, 0, 0));
        LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ir->ctx, fn, "entry");
        LLVMBasicBlockRef yes = LLVMAppendBasicBlockInContext(ir->ctx, fn, "yes");
        LLVMBasicBlockRef no = LLVMAppendBasicBlockInContext(ir->ctx, fn, "no");
        LLVMPositionBuilderAtEnd(ir->b, entry);
        LLVMValueRef branch = LLVMBuildCondBr(ir->b, LLVMConstInt(ir->t_i1, 1, 0), yes, no);
        ir_profile_branch(ir, branch, pcs[i], words[i]);
        count += LLVMGetMetadata(branch, LLVMGetMDKindIDInContext(ir->ctx, "prof", 4)) != NULL;
        LLVMPositionBuilderAtEnd(ir->b, yes);
        LLVMBuildRetVoid(ir->b);
        LLVMPositionBuilderAtEnd(ir->b, no);
        LLVMBuildRetVoid(ir->b);
    }
    char* text = LLVMPrintModuleToString(ir->mod);
    snprintf(ir_text, size, "%s", text);
    LLVMDisposeMessage(text);
    snx_ir_free(ir);
    return count;
}

int main(int argc, char** argv) {
    char error[256], text[16384], path[1200], want[1400];
    const char* dir = argc > 1 ? argv[1] : ".";
    FILE* f;
    snx_error_sink(said);
    CHECK(!snx_ir_init(error, sizeof error));

    /* none: no weights, nothing said */
    snx_ir_branch_profile("");
    CHECK(weighted(text, sizeof text) == 0 && !g_test.said[0]);

    /* a profile with a comment, a line of counts, one with a site never reached and a line of something else */
    snprintf(path, sizeof path, "%s/llvm_branch_profile.txt", dir);
    f = fopen(path, "w");
    CHECK(f);
    if (!f) return 1;
    fputs("# guest_pc instruction_word branch_taken fallthrough\n1000 d65f03c0 100 20\n3000 d65f03c0 0 0\n"
          "hot 1000\n",
          f);
    fclose(f);
    snx_ir_branch_profile(path);
    CHECK(weighted(text, sizeof text) == 1);
    CHECK(strstr(text, "!\"branch_weights\", i32 100, i32 20"));
    snprintf(want, sizeof want,
             "ir: ir_branch_profile=%s: 1 line is not a branch's address and word in hex, then how many times it was "
             "taken and not taken (line 4: \"hot 1000\"): no weights from it\n",
             path);
    CHECK(!strcmp(g_test.said, want));
    g_test.said[0] = 0;
    CHECK(weighted(text, sizeof text) == 1 &&
          !g_test.said[0]); /* read once: a second module asks again, said nothing */

    /* a file that cannot be read */
    snprintf(path, sizeof path, "%s/llvm_branch_profile_missing/none.txt", dir);
    snx_ir_branch_profile(path);
    CHECK(weighted(text, sizeof text) == 0);
    snprintf(want, sizeof want, "ir: ir_branch_profile=%s cannot be read (", path);
    CHECK(!strncmp(g_test.said, want, strlen(want)) && strstr(g_test.said, "): no branch weights\n"));

    printf("llvm_branch_profile: %s\n", g_test.failures ? "FAILED" : "weights from the profile, its bad lines said");
    return g_test.failures != 0;
}
