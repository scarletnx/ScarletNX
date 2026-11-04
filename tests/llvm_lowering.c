/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The lowering census of the A64 SIMD, FP and crypto emitters: what each op becomes on four emitter paths, against a
   checked-in summary.
     snx-test-llvm-lowering EXPECTED [--update] [--report FILE] [--ir DIR]
   Every decoder-row word the Switch target decodes (llvm_qemu_words.c) that ir_fpu.c, ir_vec.c, ir_vec_fp.c,
   ir_vec_crypto.c or ir_ldst.c's SIMD loads and stores emit becomes one step (snx_ir_static_step), optimized as a
   game's module is (snx_ir_module_optimize, the SLP vectorizer off as the setting's default), on each path:
     x86       x86-64 with every extension SnxIrFeatures has
     x86-base  x86-64 with none of them
     aarch64   AArch64 with AES, SHA2 and CRC32, the Cortex-A57's
     generic   no feature bit, as PowerPC64, RISC-V, LoongArch and MIPS64 hosts emit (riscv64's triple)
   An op's class on a path is the worst of its words': inline (no runtime helper is called), guarded (a helper on some
   paths and one path calling none: an inline fast path with a helper when its guard fails) or helper (every path
   calls one), with the helpers it calls. The step's entry call to snx_fp_env, which every generated function makes,
   is not a helper. EXPECTED holds one line per op and path; a line that differs fails the test either way, so a
   change that moves an op updates EXPECTED in the same commit (--update rewrites it). A path whose target this LLVM
   lacks is not compared, and the run says so. --report FILE writes each word's class, its shape when inline (vector,
   scalar, or lanes: extract and insert one lane at a time) and the counts of its optimized IR behind them; --ir DIR
   writes each path's optimized module as DIR/<path>.ll. The counts are of IR, cold paths included: code inspection,
   never a speed claim. */
#include "common/utilities/common.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/llvm/ir_fpu.h"
#include "common/cpu/recompiler/llvm/ir_ldst.h"
#include "common/cpu/recompiler/llvm/ir_vec.h"
#include "llvm_qemu_words.h"
#include <llvm-c/Target.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATHS   4
#define HELPERS 256 /* the helpers a word or an op calls, space-separated (add_helper stops the test past it) */
#define LINE    256

enum { CLASS_INLINE, CLASS_GUARDED, CLASS_HELPER };
enum { SHAPE_VECTOR, SHAPE_SCALAR, SHAPE_LANES };
static const char* const class_names[] = {"inline", "guarded", "helper"};
static const char* const shape_names[] = {"vector", "scalar", "lanes"};

typedef struct {
    const char* name;
    const char* triple;
    SnxIrFeatures features;
} Path;

static const Path paths[PATHS] = {
    {"x86",
     "x86_64-unknown-linux-gnu",
     {.x86 = 1,
      .sse42 = 1,
      .crc32 = 1,
      .pclmul = 1,
      .aes = 1,
      .sha_ni = 1,
      .gfni = 1,
      .fma = 1,
      .f16c = 1,
      .avx2 = 1,
      .avx512bw = 1,
      .avx512vl = 1,
      .avx512bitalg = 1}},
    {"x86-base", "x86_64-unknown-linux-gnu", {.x86 = 1}},
    {"aarch64", "aarch64-unknown-linux-gnu", {.aarch64 = 1, .arm_aes = 1, .arm_sha2 = 1, .arm_crc = 1}},
    {"generic", "riscv64-unknown-linux-gnu", {0}},
};

/* ir_ldst.c's ops that load or store V registers */
static const char* const simd_ldst[] = {"LD_mult", "ST_mult", "LD_single", "ST_single", "LD_single_repl", "LDR_v",
                                        "STR_v",   "LDR_v_i", "STR_v_i",   "LDP_v",     "STP_v",          "LD_lit_v"};
#define SIMD_LDST (sizeof(simd_ldst) / sizeof(simd_ldst[0]))

/* one word on one path */
typedef struct {
    unsigned char cls, shape;
    unsigned insns, vector, extract, insert, shuffle, calls, target;
    char helpers[HELPERS];
} Result;

typedef struct {
    SnxU32 word;
    Result at[PATHS];
} Word;

static struct {
    SnxU64 rng;
} g_llvm_lowering = {0x2545F4914F6CDD1Dull};

static uint64_t next_random(void) {
    g_llvm_lowering.rng ^= g_llvm_lowering.rng << 13;
    g_llvm_lowering.rng ^= g_llvm_lowering.rng >> 7;
    g_llvm_lowering.rng ^= g_llvm_lowering.rng << 17;
    return g_llvm_lowering.rng;
}

static int simd_ldst_index(const char* name) {
    for (size_t i = 0; i < SIMD_LDST; ++i)
        if (!strcmp(name, simd_ldst[i])) return (int)i;
    return -1;
}

/* the census's ops: an FP, AdvSIMD or crypto emitter's, or a SIMD load or store */
static int emitted(SnxU32 word, int* ldst) {
    SnxA64Insn insn;
    const char* name = lqd_switch_op_name(word);
    *ldst = -1;
    if (!name) return 0;
    SnxA64Op op = snx_a64_decode_word(word, 0, &insn);
    if (snx_ir_fpu_handles(op) || snx_ir_vec_handles(op)) return 1;
    *ldst = snx_ir_ldst_handles(op) ? simd_ldst_index(name) : -1;
    return *ldst >= 0;
}

/* The row's sample has every register field 0, so Vd, Vn and Vm alias and LLVM folds x op x: Vd 0, Vn 1 and Vm 2 where
   the word still decodes to the same name, else Vn 1 alone, else the word as it is */
static SnxU32 spread(SnxU32 word) {
    static const SnxU32 fields[2][2] = {{0x001F03FFu, (2u << 16) | (1u << 5)}, {0x000003FFu, 1u << 5}};
    const char* name = lqd_switch_op_name(word);
    for (int s = 0; s < 2; ++s) {
        SnxU32 w = (word & ~fields[s][0]) | fields[s][1];
        const char* other = lqd_switch_op_name(w);
        if (other && !strcmp(other, name)) return w;
    }
    return word;
}

/* adds name (length bytes) to set, space-separated and sorted; a set that would not fit stops the test */
static void add_helper(char* set, const char* name, size_t length) {
    size_t used = strlen(set);
    char* at = set;
    while (*at) {
        size_t word = strcspn(at, " ");
        int order = strncmp(at, name, word < length ? word : length);
        if (!order && word == length) return;
        if (order > 0 || (!order && word > length)) break;
        at += word + (at[word] == ' ');
    }
    if (used + length + 2 > HELPERS) {
        fprintf(stderr, "the helpers %s and %.*s are over HELPERS (%d bytes): raise it\n", set, (int)length, name,
                HELPERS);
        exit(1);
    }
    size_t tail = used - (size_t)(at - set);
    if (*at) {
        memmove(at + length + 1, at, tail + 1);
        memcpy(at, name, length);
        at[length] = ' ';
    } else {
        if (at > set && at[-1] != ' ') *at++ = ' ';
        memcpy(at, name, length);
        at[length] = 0;
    }
}

static int vector_opcode(LLVMOpcode op) {
    switch (op) {
        case LLVMAdd:
        case LLVMFAdd:
        case LLVMSub:
        case LLVMFSub:
        case LLVMMul:
        case LLVMFMul:
        case LLVMUDiv:
        case LLVMSDiv:
        case LLVMFDiv:
        case LLVMURem:
        case LLVMSRem:
        case LLVMFRem:
        case LLVMShl:
        case LLVMLShr:
        case LLVMAShr:
        case LLVMAnd:
        case LLVMOr:
        case LLVMXor:
        case LLVMFNeg:
        case LLVMICmp:
        case LLVMFCmp:
        case LLVMSelect:
        case LLVMTrunc:
        case LLVMZExt:
        case LLVMSExt:
        case LLVMFPToUI:
        case LLVMFPToSI:
        case LLVMUIToFP:
        case LLVMSIToFP:
        case LLVMFPTrunc:
        case LLVMFPExt:
        case LLVMCall: return 1;
        default: return 0;
    }
}

/* r from fn's optimized IR; -1 when it has no blocks */
static int classify(LLVMValueRef fn, Result* r) {
    static const char* const targets[] = {"llvm.x86.",   "llvm.aarch64.",   "llvm.ppc.",
                                          "llvm.riscv.", "llvm.loongarch.", "llvm.mips."};
    unsigned count = LLVMCountBasicBlocks(fn);
    if (!count) return -1;
    LLVMBasicBlockRef* blocks = (LLVMBasicBlockRef*)malloc(count * (sizeof(LLVMBasicBlockRef) + sizeof(unsigned) + 2));
    if (!blocks) return -1;
    unsigned* stack = (unsigned*)(blocks + count);
    unsigned char* calls = (unsigned char*)(stack + count);
    unsigned char* seen = calls + count;
    LLVMGetBasicBlocks(fn, blocks);
    memset(r, 0, sizeof(*r));
    memset(calls, 0, 2 * (size_t)count);
    for (unsigned b = 0; b < count; ++b)
        for (LLVMValueRef i = LLVMGetFirstInstruction(blocks[b]); i; i = LLVMGetNextInstruction(i)) {
            LLVMOpcode op = LLVMGetInstructionOpcode(i);
            ++r->insns;
            if (op == LLVMExtractElement) ++r->extract;
            if (op == LLVMInsertElement) ++r->insert;
            if (op == LLVMShuffleVector) ++r->shuffle;
            if (vector_opcode(op) && LLVMGetTypeKind(LLVMTypeOf(i)) == LLVMVectorTypeKind) ++r->vector;
            if (op != LLVMCall) continue;
            LLVMValueRef callee = LLVMGetCalledValue(i);
            if (LLVMIsAInlineAsm(callee)) continue; /* native instructions, not an indirect helper call */
            size_t length = 0;
            const char* name = LLVMIsAFunction(callee) ? LLVMGetValueName2(callee, &length) : "(indirect)";
            if (!LLVMIsAFunction(callee)) length = strlen(name);
            if (length > 5 && !strncmp(name, "llvm.", 5)) {
                for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); ++t)
                    if (!strncmp(name, targets[t], strlen(targets[t]))) ++r->target;
                continue;
            }
            if (length == 10 && !strncmp(name, "snx_fp_env", 10)) continue;
            calls[b] = 1;
            ++r->calls;
            add_helper(r->helpers, name, length);
        }
    if (!r->calls)
        r->cls = CLASS_INLINE;
    else {
        /* a path from the entry to a return through blocks that call no helper; each block pushed once */
        unsigned top = 0;
        r->cls = CLASS_HELPER;
        if (!calls[0]) {
            seen[0] = 1;
            stack[top++] = 0;
        }
        while (top) {
            LLVMValueRef term = LLVMGetBasicBlockTerminator(blocks[stack[--top]]);
            if (LLVMGetInstructionOpcode(term) == LLVMRet) {
                r->cls = CLASS_GUARDED;
                break;
            }
            for (unsigned s = 0; s < LLVMGetNumSuccessors(term); ++s) {
                LLVMBasicBlockRef next = LLVMGetSuccessor(term, s);
                for (unsigned k = 0; k < count; ++k)
                    if (blocks[k] == next && !seen[k] && !calls[k]) {
                        seen[k] = 1;
                        stack[top++] = k;
                    }
            }
        }
    }
    r->shape = r->extract + r->insert >= 4 && r->vector < 2 ? SHAPE_LANES
               : r->vector || r->shuffle || r->target       ? SHAPE_VECTOR
                                                            : SHAPE_SCALAR;
    free(blocks);
    return 0;
}

/* A native FP-environment helper uses inline assembly on AArch64. Keep that distinct from a real function-pointer
   call, even when the census runs on another host. Both controls retain their calls through optimization. */
static int classify_controls(void) {
    char error[1024];
    SnxIr* ir = snx_ir_new("lowering_controls", '2', error, sizeof error);
    if (!ir) return snx_error("%s", error);
    LLVMTypeRef plain = LLVMFunctionType(ir->t_void, NULL, 0, 0);
    LLVMValueRef assembly = LLVMAddFunction(ir->mod, "assembly", plain);
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, assembly, "entry"));
    LLVMValueRef body = LLVMGetInlineAsm(plain, "", 0, "", 0, 1, 0, LLVMInlineAsmDialectATT, 0);
    LLVMBuildCall2(ir->b, plain, body, NULL, 0, "");
    LLVMBuildRetVoid(ir->b);
    LLVMValueRef indirect = LLVMAddFunction(ir->mod, "indirect", LLVMFunctionType(ir->t_void, &ir->t_ptr, 1, 0));
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, indirect, "entry"));
    LLVMBuildCall2(ir->b, plain, LLVMGetParam(indirect, 0), NULL, 0, "");
    LLVMBuildRetVoid(ir->b);
    int failed = snx_ir_module_optimize(ir, ir->mod, '2', error, sizeof error);
    Result a, b;
    if (!failed) {
        failed = classify(assembly, &a) || classify(indirect, &b) || a.cls != CLASS_INLINE || a.calls ||
                 b.cls != CLASS_HELPER || b.calls != 1 || strcmp(b.helpers, "(indirect)");
    }
    snx_ir_free(ir);
    return failed ? snx_error("inline-assembly and indirect-call census controls differ") : 0;
}

/* every word's result on path p: 1 when this LLVM has the path's target, 0 when not (said), -1 on an error */
static int run_path(unsigned p, Word* words, size_t count, const char* ir_dir) {
    char error[1024];
    SnxIr* ir = snx_ir_new_target("lowering", '2', paths[p].triple, error, sizeof error);
    if (!ir) {
        printf("path %s (%s) not compared: %s\n", paths[p].name, paths[p].triple, error);
        return 0;
    }
    ir->features = &paths[p].features;
    for (size_t i = 0; i < count; ++i) {
        char name[32];
        snprintf(name, sizeof name, "S_%zu", i);
        snx_ir_static_step(ir, words[i].word, 0x1000, 0, name);
        if (ir->fallbacks) {
            fprintf(stderr, "path %s: no emitter for %s (%08x)\n", paths[p].name, lqd_switch_op_name(words[i].word),
                    (unsigned)words[i].word);
            snx_ir_free(ir);
            return -1;
        }
    }
    if (snx_ir_module_optimize(ir, ir->mod, '2', error, sizeof error)) {
        fprintf(stderr, "path %s: %s\n", paths[p].name, error);
        snx_ir_free(ir);
        return -1;
    }
    if (ir_dir) {
        char* path = snx_format("%s/%s.ll", ir_dir, paths[p].name);
        char* message = NULL;
        int failed = LLVMPrintModuleToFile(ir->mod, path, &message);
        if (failed) fprintf(stderr, "cannot write %s: %s\n", path, message ? message : "");
        LLVMDisposeMessage(message);
        free(path);
        if (failed) {
            snx_ir_free(ir);
            return -1;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        char name[32];
        snprintf(name, sizeof name, "S_%zu", i);
        LLVMValueRef fn = LLVMGetNamedFunction(ir->mod, name);
        if (!fn || classify(fn, &words[i].at[p])) {
            fprintf(stderr, "path %s: %s (%s) has no body after the passes\n", paths[p].name, name,
                    lqd_switch_op_name(words[i].word));
            snx_ir_free(ir);
            return -1;
        }
    }
    snx_ir_free(ir);
    return 1;
}

static int by_name(const void* a, const void* b) {
    const Word *x = (const Word*)a, *y = (const Word*)b;
    int c = strcmp(lqd_switch_op_name(x->word), lqd_switch_op_name(y->word));
    return c ? c : (x->word > y->word) - (x->word < y->word);
}

static int by_line(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

/* "NAME PATH": a line's key, the rest its value */
static size_t key_length(const char* line) {
    const char* space = strchr(line, ' ');
    space = space ? strchr(space + 1, ' ') : NULL;
    return space ? (size_t)(space - line) : strlen(line);
}

/* one line per op and path that ran, as EXPECTED has them (single spaces): malloc'd, *lines set; tally[p][class]
   counts the ops of each class on path p */
static char** summary(const Word* words, size_t count, const int* ran, size_t* lines, unsigned tally[PATHS][3]) {
    char** out = (char**)malloc((count * PATHS + 1) * sizeof(char*));
    *lines = 0;
    if (!out) return NULL;
    for (size_t i = 0; i < count;) {
        const char* name = lqd_switch_op_name(words[i].word);
        size_t end = i;
        while (end < count && !strcmp(lqd_switch_op_name(words[end].word), name)) ++end;
        for (unsigned p = 0; p < PATHS; ++p) {
            if (ran[p] != 1) continue;
            unsigned cls = 0;
            char helpers[HELPERS] = {0};
            for (size_t k = i; k < end; ++k) {
                if (words[k].at[p].cls > cls) cls = words[k].at[p].cls;
                for (const char* t = words[k].at[p].helpers; *t;) {
                    size_t length = strcspn(t, " ");
                    add_helper(helpers, t, length);
                    t += length + (t[length] == ' ');
                }
            }
            ++tally[p][cls];
            out[(*lines)++] =
                snx_format("%s %s %s%s%s", name, paths[p].name, class_names[cls], *helpers ? " " : "", helpers);
        }
        i = end;
    }
    return out;
}

/* EXPECTED's lines, comments and blanks left out and spaces made single */
static char** read_expected(const char* path, size_t* lines) {
    size_t length = 0, capacity = 1024;
    char* text = (char*)snx_read_file(path, &length);
    char** out = (char**)malloc(capacity * sizeof(char*));
    *lines = 0;
    if (!text || !out) {
        free(text);
        free(out);
        return NULL;
    }
    for (char* line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char normal[LINE] = {0};
        size_t used = 0;
        for (const char* t = line; *t && *t != '#';) {
            while (*t == ' ' || *t == '\t' || *t == '\r') ++t;
            const char* start = t;
            while (*t && *t != ' ' && *t != '\t' && *t != '\r' && *t != '#') ++t;
            if (t == start) continue;
            if (used + (size_t)(t - start) + 2 > sizeof normal) {
                fprintf(stderr, "%s: a line over %d bytes: %.60s...\n", path, LINE, line);
                for (size_t i = 0; i < *lines; ++i) free(out[i]);
                free(out);
                free(text);
                *lines = 0;
                return NULL;
            }
            used += (size_t)snprintf(normal + used, sizeof normal - used, "%s%.*s", used ? " " : "", (int)(t - start),
                                     start);
        }
        if (!used) continue;
        if (*lines == capacity) {
            capacity *= 2;
            out = (char**)snx_realloc(out, capacity * sizeof(char*));
        }
        out[(*lines)++] = snx_strdup(normal);
    }
    free(text);
    return out;
}

static int path_ran(const char* line, const int* ran) {
    const char* space = strchr(line, ' ');
    if (!space) return 0;
    size_t key = key_length(line), at = (size_t)(space - line) + 1;
    for (unsigned p = 0; p < PATHS; ++p)
        if (strlen(paths[p].name) == key - at && !strncmp(line + at, paths[p].name, key - at)) return ran[p] == 1;
    return 0;
}

/* the lines that differ, each said: their number */
static size_t compare(char** got, size_t ngot, char** want, size_t nwant, const int* ran, const char* expected) {
    size_t g = 0, w = 0, differ = 0;
    qsort(got, ngot, sizeof(char*), by_line);
    qsort(want, nwant, sizeof(char*), by_line);
    while (g < ngot || w < nwant) {
        if (w < nwant && !path_ran(want[w], ran)) {
            ++w;
            continue;
        }
        int order = g == ngot ? 1 : w == nwant ? -1 : 0;
        size_t kg = g < ngot ? key_length(got[g]) : 0, kw = w < nwant ? key_length(want[w]) : 0;
        if (!order) {
            order = strncmp(got[g], want[w], kg < kw ? kg : kw);
            if (!order) order = (kg > kw) - (kg < kw);
        }
        if (order < 0) {
            printf("%.*s: %s here, not in %s\n", (int)kg, got[g], got[g] + kg + 1, expected);
            ++differ;
            ++g;
        } else if (order > 0) {
            printf("%.*s: %s in %s, and no word here decodes to it\n", (int)kw, want[w], want[w] + kw + 1, expected);
            ++differ;
            ++w;
        } else {
            if (strcmp(got[g], want[w])) {
                printf("%.*s: %s here, %s in %s\n", (int)kg, got[g], got[g] + kg + 1, want[w] + kw + 1, expected);
                ++differ;
            }
            ++g;
            ++w;
        }
    }
    return differ;
}

static int write_summary(const char* path, char** lines, size_t count) {
    FILE* f = fopen(path, "w");
    if (!f) return -1;
    fprintf(
        f, "# snx-test-llvm-lowering's summary (llvm_lowering.c says how it is made): each A64 SIMD, FP and crypto op\n"
           "# the Switch target decodes, on each emitter path, as inline, guarded (inline with a helper when a guard\n"
           "# fails) or helper (always), and the helpers it calls. --update rewrites it.\n"
           "# op               path      class    helpers\n");
    /* summary()'s lines, "NAME PATH CLASS" and the helpers when there are some, in columns */
    for (size_t i = 0; i < count; ++i) {
        const char* path_name = strchr(lines[i], ' ') + 1;
        const char* cls = strchr(path_name, ' ') + 1;
        const char* helpers = strchr(cls, ' ');
        int name_length = (int)(path_name - 1 - lines[i]), path_length = (int)(cls - 1 - path_name);
        if (helpers)
            fprintf(f, "%-18.*s %-9.*s %-8.*s %s\n", name_length, lines[i], path_length, path_name,
                    (int)(helpers - cls), cls, helpers + 1);
        else
            fprintf(f, "%-18.*s %-9.*s %s\n", name_length, lines[i], path_length, path_name, cls);
    }
    return fclose(f);
}

static int write_report(const char* path, const Word* words, size_t count, const int* ran) {
    FILE* f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "word\tname\tpath\tclass\tshape\tinsns\tvector\textract\tinsert\tshuffle\ttarget\tcalls\thelpers\n");
    for (size_t i = 0; i < count; ++i)
        for (unsigned p = 0; p < PATHS; ++p) {
            const Result* r = &words[i].at[p];
            if (ran[p] != 1) continue;
            fprintf(f, "%08x\t%s\t%s\t%s\t%s\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%s\n", (unsigned)words[i].word,
                    lqd_switch_op_name(words[i].word), paths[p].name, class_names[r->cls], shape_names[r->shape],
                    r->insns, r->vector, r->extract, r->insert, r->shuffle, r->target, r->calls, r->helpers);
        }
    return fclose(f);
}

int main(int argc, char** argv) {
    const char *expected = NULL, *report = NULL, *ir_dir = NULL;
    int update = 0, ran[PATHS] = {0}, any = 0;
    char error[1024];
    for (int a = 1; a < argc; ++a) {
        if (!strcmp(argv[a], "--update"))
            update = 1;
        else if (!strcmp(argv[a], "--report") && a + 1 < argc)
            report = argv[++a];
        else if (!strcmp(argv[a], "--ir") && a + 1 < argc)
            ir_dir = argv[++a];
        else if (argv[a][0] != '-' && !expected)
            expected = argv[a];
        else {
            fprintf(stderr,
                    "snx-test-llvm-lowering: %s is not an option (--update, --report FILE, --ir DIR) or the one "
                    "EXPECTED file\n",
                    argv[a]);
            return 2;
        }
    }
    if (!expected) {
        fprintf(stderr, "usage: snx-test-llvm-lowering EXPECTED [--update] [--report FILE] [--ir DIR]\n");
        return 2;
    }
    if (snx_ir_init(error, sizeof error)) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    snx_ir_slp_vectorizer(0);
    if (classify_controls()) return 1;

    uint32_t* rows = NULL;
    size_t nrows = lqd_row_words(1, next_random, &rows), count = 0;
    Word* words = (Word*)malloc((nrows ? nrows : 1) * sizeof(Word));
    unsigned char ldst_seen[SIMD_LDST] = {0};
    if (!words) return 1;
    for (size_t i = 0; i < nrows; ++i) {
        int ldst;
        if (!emitted(rows[i], &ldst)) continue;
        if (ldst >= 0) ldst_seen[ldst] = 1;
        memset(&words[count], 0, sizeof(Word));
        words[count++].word = spread(rows[i]);
    }
    free(rows);
    for (size_t i = 0; i < SIMD_LDST; ++i)
        if (!ldst_seen[i]) {
            fprintf(stderr,
                    "the SIMD load or store %s is no row the Switch target decodes and ir_ldst.c emits: "
                    "llvm_lowering.c's list is out of date\n",
                    simd_ldst[i]);
            return 1;
        }
    qsort(words, count, sizeof(Word), by_name);
    for (unsigned p = 0; p < PATHS; ++p) {
        ran[p] = run_path(p, words, count, ir_dir);
        if (ran[p] < 0) return 1;
        any |= ran[p];
    }
    if (!any) {
        printf("this LLVM has none of the paths' targets: nothing compared\n");
        return 77;
    }
    if (report && write_report(report, words, count, ran)) {
        fprintf(stderr, "cannot write %s\n", report);
        return 1;
    }
    size_t ngot = 0, nwant = 0;
    unsigned tally[PATHS][3] = {{0}};
    char** got = summary(words, count, ran, &ngot, tally);
    if (!got) return 1;
    for (unsigned p = 0; p < PATHS; ++p)
        if (ran[p] == 1)
            printf("%-9s %u inline, %u guarded, %u helper\n", paths[p].name, tally[p][CLASS_INLINE],
                   tally[p][CLASS_GUARDED], tally[p][CLASS_HELPER]);
    if (update) {
        for (unsigned p = 0; p < PATHS; ++p)
            if (ran[p] != 1) {
                fprintf(stderr, "--update needs every path, and %s did not run: %s not written\n", paths[p].name,
                        expected);
                return 1;
            }
        qsort(got, ngot, sizeof(char*), by_line);
        if (write_summary(expected, got, ngot)) {
            fprintf(stderr, "cannot write %s\n", expected);
            return 1;
        }
        printf("%zu words, %zu lines written to %s\n", count, ngot, expected);
        return 0;
    }
    char** want = read_expected(expected, &nwant);
    if (!want) {
        fprintf(stderr, "cannot read %s\n", expected);
        return 1;
    }
    size_t differ = compare(got, ngot, want, nwant, ran, expected);
    printf("%zu words, %zu op lines: %zu differ from %s%s\n", count, ngot, differ, expected,
           differ ? "; a change that moves an op updates it in the same commit (--update)" : "");
    for (size_t i = 0; i < ngot; ++i) free(got[i]);
    for (size_t i = 0; i < nwant; ++i) free(want[i]);
    free(got);
    free(want);
    free(words);
    return differ ? 1 : 0;
}
