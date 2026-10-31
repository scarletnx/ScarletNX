/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* LLVM Internals (settings.txt's llvm table, nx/runtime/driver/llvm_tuning.c): with every pass on, the recompile runs
   LLVM's own default<O?>; a pass turned off is cut out of the pipeline opt printed, and what is left is a pipeline LLVM
   runs, at every level; with every pass of a level off what is left runs analyses only, and a level left with nothing
   at all is refused with its reason. Without an opt of
   this LLVM where it was built there are no pipelines, and nothing to check but -Os and -Oz, which every build makes as
   LLVM 23 does: O2 over functions marked optsize, and minsize for z */
#include "common/cpu/recompiler/llvm/ir.h"
#include "nx/loader/module_llvm.h"
#include "nx/runtime/settings_llvm.h"
#include "snx_settings.h"
#include <llvm-c/Analysis.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

/* whether LLVM runs pipeline over a module with one function that has something to optimize */
static int runs(const char* pipeline) {
    LLVMContextRef ctx = LLVMContextCreate();
    LLVMModuleRef mod = LLVMModuleCreateWithNameInContext("tuning", ctx);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx), args[1] = {i32};
    LLVMValueRef fn = LLVMAddFunction(mod, "f", LLVMFunctionType(i32, args, 1, 0));
    LLVMBuilderRef b = LLVMCreateBuilderInContext(ctx);
    LLVMPassBuilderOptionsRef options = LLVMCreatePassBuilderOptions();
    LLVMErrorRef error;
    LLVMPositionBuilderAtEnd(b, LLVMAppendBasicBlockInContext(ctx, fn, "entry"));
    LLVMBuildRet(b, LLVMBuildAdd(b, LLVMBuildAdd(b, LLVMGetParam(fn, 0), LLVMConstInt(i32, 1, 0), ""),
                                 LLVMConstInt(i32, 2, 0), ""));
    error = LLVMRunPasses(mod, pipeline, NULL, options);
    if (error) {
        char* message = LLVMGetErrorMessage(error);
        fprintf(stderr, "LLVM refuses %.200s...: %s\n", pipeline, message);
        LLVMDisposeErrorMessage(message);
    }
    LLVMDisposePassBuilderOptions(options);
    LLVMDisposeBuilder(b);
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    return !error;
}

/* the pass's name in pipeline as a pass of its own, not part of another's (aggressive-instcombine's) */
static int has_pass(const char* pipeline, const char* name) {
    const char* p;
    size_t n = strlen(name);
    for (p = strstr(pipeline, name); p; p = strstr(p + 1, name))
        if ((p == pipeline || strchr(",(", p[-1])) && strchr(",()<", p[n])) return 1;
    return 0;
}

static void set_all(const char* value) {
    const IniOpt* o;
    for (o = g_llvm_ini; o->key; ++o)
        if (!strncmp(o->key, "llvm_pass_", 10)) ini_read(o, value);
}

static int has_attribute(LLVMValueRef fn, const char* name) {
    return LLVMGetEnumAttributeAtIndex(fn, LLVMAttributeFunctionIndex,
                                       LLVMGetEnumAttributeKindForName(name, strlen(name))) != NULL;
}
/* -Os and -Oz, which LLVM 23 has only as O2 over functions marked optsize (and minsize): an object made at each, its
   function marked so (ir.c) */
static void size_levels(void) {
    static const char levels[] = "sz";
    char error[256];
    int i;
    CHECK(!snx_ir_init(error, sizeof error));
    for (i = 0; levels[i]; ++i) {
        SnxIr* ir = snx_ir_new("size", levels[i], error, sizeof error);
        LLVMTypeRef params[1];
        LLVMValueRef fn;
        unsigned char* data = NULL;
        size_t size = 0;
        CHECK(ir != NULL);
        if (!ir) continue;
        params[0] = ir->t_i64;
        fn = LLVMAddFunction(ir->mod, "f", LLVMFunctionType(ir->t_i64, params, 1, 0));
        LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, fn, "entry"));
        LLVMBuildRet(ir->b, LLVMBuildAdd(ir->b, LLVMGetParam(fn, 0), LLVMConstInt(ir->t_i64, 1, 0), ""));
        if (snx_ir_object(ir, levels[i], &data, &size, error, sizeof error))
            fprintf(stderr, "-O%c: %s\n", levels[i], error);
        CHECK(data && size);
        CHECK(has_attribute(fn, "optsize") && has_attribute(fn, "minsize") == (levels[i] == 'z'));
        free(data);
        snx_ir_free(ir);
    }
}

/* the tuning's text, which the unit cache keys objects with (nx/loader/unit_cache.c): the pipeline that runs and every
   LLVM Internals value, so an option set changes it */
static void tuning_text(void) {
    const IniOpt* threshold = ini_find(g_llvm_ini, "llvm_inliner_threshold");
    const SnxIrTuning* t;
    char error[256], *before;
    int saved;
    CHECK(threshold != NULL);
    if (!threshold) return;
    saved = *(const int*)threshold->var;
    t = snx_llvm_tuning('2', error, sizeof error);
    CHECK(t && t->text && !strncmp(t->text, "pipeline=", 9) && strstr(t->text, "\nllvm_inliner_threshold="));
    before = t && t->text ? snx_strdup(t->text) : NULL;
    ini_read(threshold, "50");
    t = snx_llvm_tuning('2', error, sizeof error);
    CHECK(t && t->text && before && strstr(t->text, "\nllvm_inliner_threshold=50\n") && strcmp(before, t->text));
    *(int*)threshold->var = saved;
    t = snx_llvm_tuning('2', error, sizeof error);
    CHECK(t && t->text && before && !strcmp(before, t->text));
    free(before);
}

/* llvm_batch: full runs the level's own pipeline, ipo SNX_IR_IPO_PASSES in its place, a pipeline LLVM runs; either
   way each function's cleanup as it is emitted is SNX_IR_CLEAN_PASSES, and a pass turned off is cut out of both. The
   tuning's text, which keys the unit cache, differs: an ipo unit is never a full one */
static void batch(void) {
    const IniOpt* option = ini_find(g_recompile_ini, "llvm_batch");
    const IniOpt* instcombine = ini_find(g_llvm_ini, "llvm_pass_instcombine");
    const SnxIrTuning* t;
    char error[256], *full = NULL, clean[256];
    CHECK(option != NULL && instcombine != NULL && !strcmp(g_settings.llvm_batch, "full"));
    if (!option || !instcombine) return;
    t = snx_llvm_tuning('2', error, sizeof error);
    CHECK(t && !t->pipeline && t->clean && !strcmp(t->clean, SNX_IR_CLEAN_PASSES));
    if (t && t->text) full = snx_strdup(t->text);
    ini_read(option, "ipo");
    t = snx_llvm_tuning('2', error, sizeof error);
    CHECK(t && t->pipeline && !strcmp(t->pipeline, SNX_IR_IPO_PASSES) && runs(t->pipeline));
    CHECK(t && t->clean && !strcmp(t->clean, SNX_IR_CLEAN_PASSES));
    CHECK(t && t->text && full &&
          !strncmp(t->text, "pipeline=" SNX_IR_IPO_PASSES "\n", 10 + strlen(SNX_IR_IPO_PASSES)) &&
          strcmp(t->text, full));
    ini_read(instcombine, "0");
    t = snx_llvm_tuning('2', error, sizeof error);
    CHECK(t && t->pipeline && !has_pass(t->pipeline, "instcombine") && runs(t->pipeline));
    CHECK(t && t->clean && !has_pass(t->clean, "instcombine") && has_pass(t->clean, "sroa"));
    if (t && t->clean) {
        snprintf(clean, sizeof clean, "function(%s)", t->clean);
        CHECK(runs(clean));
    }
    ini_read(instcombine, "1");
    ini_read(option, "full");
    t = snx_llvm_tuning('2', error, sizeof error);
    CHECK(t && !t->pipeline && t->text && full && !strcmp(t->text, full));
    free(full);
}

int main(void) {
    static const char levels[] = "0123sz";
    const IniOpt* instcombine = ini_find(g_llvm_ini, "llvm_pass_instcombine");
    const SnxIrTuning* t;
    char error[256];
    int i;
    size_levels();
    tuning_text();
    batch();
    if (failures) return 1;
    if (!settings_llvm_pipeline('2')) {
        puts("SKIP no pipelines: no opt of this LLVM ran where it was built");
        return 0;
    }
    CHECK(instcombine && settings_llvm_pass(&g_settings, "instcombine", 11) == 1);
    CHECK(settings_llvm_pass(&g_settings, "no-such-pass", 12) == -1);
    for (i = 0; levels[i]; ++i) { /* every pass on: LLVM's own pipeline */
        t = snx_llvm_tuning(levels[i], error, sizeof error);
        CHECK(t && !t->pipeline && t->builder && t->machine);
        CHECK(runs(settings_llvm_pipeline(levels[i])));
    }
    ini_read(instcombine, "0"); /* one off: cut out of each level that has it, aggressive-instcombine kept */
    /* through the table, not its member: settingsgen makes the member only where an opt printed pipelines */
    CHECK(settings_llvm_pass(&g_settings, "instcombine", 11) == 0);
    for (i = 0; levels[i]; ++i) {
        const char* opt = settings_llvm_pipeline(levels[i]);
        t = snx_llvm_tuning(levels[i], error, sizeof error);
        CHECK(t != NULL);
        if (!t) continue;
        CHECK(has_pass(opt, "instcombine") == (t->pipeline != NULL));
        if (!t->pipeline) continue;
        CHECK(!has_pass(t->pipeline, "instcombine") && runs(t->pipeline));
        CHECK(has_pass(opt, "aggressive-instcombine") == has_pass(t->pipeline, "aggressive-instcombine"));
    }
    set_all("0"); /* every pass off: what is left (require<> and invalidate<>, analyses) runs, or the level has none */
    for (i = 0; levels[i]; ++i) {
        t = snx_llvm_tuning(levels[i], error, sizeof error);
        CHECK(t ? t->pipeline && !has_pass(t->pipeline, "instcombine") && !has_pass(t->pipeline, "sroa") &&
                      runs(t->pipeline)
                : strstr(error, "every pass") != NULL);
    }
    set_all("1");
    t = snx_llvm_tuning('2', error, sizeof error);
    CHECK(t && !t->pipeline);
    /* a level with no pipeline in this build (its opt failed on it; x, which is none, stands for it here): LLVM's own
       with every pass on, and with one off refused naming the pass and the level, never LLVM's own with it still in */
    t = snx_llvm_tuning('x', error, sizeof error);
    CHECK(t && !t->pipeline);
    ini_read(instcombine, "0");
    CHECK(!snx_llvm_tuning('x', error, sizeof error) && strstr(error, "llvm_pass_instcombine") && strstr(error, "-Ox"));
    for (i = 0; levels[i]; ++i)
        if (!settings_llvm_pipeline(levels[i])) CHECK(!snx_llvm_tuning(levels[i], error, sizeof error));
    ini_read(instcombine, "1");
    if (failures) return 1;
    puts("PASS LLVM Internals cut passes out of LLVM's pipelines, which LLVM still runs");
    return 0;
}
