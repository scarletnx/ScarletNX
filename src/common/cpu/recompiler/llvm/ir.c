/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/aarch64/cpu.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/BitReader.h>
#include <llvm/Config/llvm-config.h>
#include <llvm-c/Support.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the PowerPC64 CPU the runtime library is built for (cmake/cross.cmake), and a module for another host's */
#ifdef SNX_PPC64_LLVM_CPU
#define SNX_PPC_CPU SNX_PPC64_LLVM_CPU
#else
#define SNX_PPC_CPU "pwr8"
#endif

/* ---- setup ---- */

SnxIrFeatures snx_ir_host;
/* snx_ir_slp's */
static struct {
    int slp_off;
    int domain_off; /* snx_ir_init's, of X86DomainReassignment */
    int fpsr_loose; /* snx_ir_fpsr_flags(0)'s */
    int volatile_memory; /* snx_ir_volatile_memory's */
} g_ir;
static const SnxIrFeatures generic_features;

static int has_feature(const char* features, const char* name) {
    size_t n = strlen(name);
    for (const char* p = features; (p = strstr(p, name)) != NULL; p += n)
        if (p > features && p[-1] == '+' && (p[n] == ',' || !p[n])) return 1;
    return 0;
}

/* RISC-V's and LoongArch's: their objects go through the module's own linker (link_elf.c), which takes no relaxation */
static int riscv_triple(const char* triple) { return !strncmp(triple, "riscv64", 7); }
static int loongarch_triple(const char* triple) { return !strncmp(triple, "loongarch64", 11); }

void snx_ir_slp_vectorizer(int on) {
    /* the pass builder's SLPVectorization leaves default<O?>'s slp-vectorizer in (LLVM 22): its own switch stops it */
    static const char* const off[] = {"scarletnx", "-vectorize-slp=false"};
    if (on || g_ir.slp_off) return;
    LLVMParseCommandLineOptions(2, off, NULL);
    g_ir.slp_off = 1;
}
int snx_ir_slp(void) { return !g_ir.slp_off; }
void snx_ir_fpsr_flags(int exact) { g_ir.fpsr_loose = !exact; }
int snx_ir_fpsr_exact(void) { return !g_ir.fpsr_loose; }
void snx_ir_volatile_memory(int on) { g_ir.volatile_memory = on != 0; }
int snx_ir_volatile(void) { return g_ir.volatile_memory; }
int snx_ir_init(char* error, size_t error_size) {
    /* the asm parser for the runtime library's inline assembly (rtlib.c's divq, cpu.h's compiler barrier) */
    if (LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter() || LLVMInitializeNativeAsmParser()) {
        snprintf(error, error_size, "LLVM has no code generator for this host");
        return -1;
    }
    /* x86-64, AArch64, PowerPC64, RISC-V and LoongArch hosts: the guest's barriers, acquires, releases and exclusives
       are fences and seq_cst atomics (cpu.h), so its plain accesses need no ordering of their own from the host. POWER
       is not multi-copy atomic as ARMv8 is, which only a program ordering plain accesses on three threads without
       barriers could see; its seq_cst fence (hwsync) is cumulative, as DMB is */
    char* triple = LLVMGetDefaultTargetTriple();
    int x86 = !strncmp(triple, "x86_64", 6);
    int arm = !strncmp(triple, "aarch64", 7) || !strncmp(triple, "arm64", 5); /* Apple names AArch64 arm64 */
    int ppc = !strncmp(triple, "powerpc64", 9) || !strncmp(triple, "ppc64", 5);
    int other = riscv_triple(triple) || loongarch_triple(triple);
    if (!x86 && !arm && !ppc && !other)
        snprintf(error, error_size,
                 "the LLVM backend targets x86-64, AArch64, PowerPC64, RISC-V and LoongArch hosts, this one is %s",
                 triple);
    LLVMDisposeMessage(triple);
    if (x86 && !g_ir.domain_off) {
        /* X86DomainReassignment, which moves chains of i1 and i8 logic into AVX-512 mask registers, takes time
           quadratic in their size on a function's NZCV and FP fast-path tests: 23 s of one game function's 26 s of
           code generation, for the same code. Once per process: LLVM's command line is the process's */
        static const char* const off[] = {"scarletnx", "-disable-x86-domain-reassignment"};
        LLVMParseCommandLineOptions(2, off, NULL);
        g_ir.domain_off = 1;
    }
    if (x86 && !snx_ir_host.x86) {
        char* f = LLVMGetHostCPUFeatures();
        snx_ir_host.sse42 = has_feature(f, "sse4.2");
        snx_ir_host.crc32 = snx_ir_host.sse42 && has_feature(f, "crc32");
        snx_ir_host.pclmul = has_feature(f, "pclmul");
        snx_ir_host.aes = has_feature(f, "aes");
        snx_ir_host.sha_ni = has_feature(f, "sha");
        snx_ir_host.gfni = has_feature(f, "gfni");
        snx_ir_host.fma = has_feature(f, "fma");
        snx_ir_host.f16c = has_feature(f, "f16c");
        snx_ir_host.avx2 = has_feature(f, "avx2");
        snx_ir_host.avx512bw = has_feature(f, "avx512bw");
        snx_ir_host.avx512vl = has_feature(f, "avx512vl");
        snx_ir_host.avx512bitalg = has_feature(f, "avx512bitalg");
        LLVMDisposeMessage(f);
        snx_ir_host.x86 = 1; /* last: snx_ir_init may run again, the features stay as read */
    }
    if (arm && !snx_ir_host.aarch64) {
        char* f = LLVMGetHostCPUFeatures();
        snx_ir_host.arm_aes = has_feature(f, "aes") || has_feature(f, "crypto");
        snx_ir_host.arm_sha2 = has_feature(f, "sha2") || has_feature(f, "crypto");
        snx_ir_host.arm_crc = has_feature(f, "crc");
        LLVMDisposeMessage(f);
        snx_ir_host.aarch64 = 1; /* last, as x86 above */
    }
    return x86 || arm || ppc || other ? 0 : -1;
}

static LLVMCodeGenOptLevel codegen_level(char opt) {
    switch (opt) {
        case '0': return LLVMCodeGenLevelNone;
        case '1': return LLVMCodeGenLevelLess;
        case '3': return LLVMCodeGenLevelAggressive;
        default: return LLVMCodeGenLevelDefault;
    }
}

/* The host's triple, for objects the JIT loader loads when jit is nonzero. The loader reads ELF (and Windows' COFF),
   and Darwin's objects are Mach-O: there the triple names ELF as its object format (arm64-apple-darwin<version>-elf),
   and the code stays Darwin's. LLVM's AArch64 backend picks the calling convention, the reserved registers and the
   library calls by the operating system, not by the object format: DarwinPCS (variadic arguments on the stack, small
   arguments extended by the caller), x18 and x29 never allocated, bzero and memset_pattern16 for memset (libc.inc).
   Only the sections, relocations and symbol names (no leading underscore) are ELF's */
static char* host_triple(int jit) {
    char* triple = LLVMGetDefaultTargetTriple();
#ifdef __APPLE__
    if (jit && !strstr(triple, "-elf")) {
        size_t n = strlen(triple);
        char* elf = (char*)malloc(n + sizeof("-elf"));
        if (elf) {
            memcpy(elf, triple, n);
            memcpy(elf + n, "-elf", sizeof("-elf"));
            LLVMDisposeMessage(triple);
            triple = LLVMCreateMessage(elf);
            free(elf);
        }
    }
#else
    (void)jit;
#endif
    return triple;
}

static SnxIr* new_ir(const char* name, char opt, const char* target_triple, int jit, char* error, size_t error_size);

SnxIr* snx_ir_new(const char* name, char opt, char* error, size_t error_size) {
    return new_ir(name, opt, NULL, 0, error, error_size);
}

SnxIr* snx_ir_new_jit(const char* name, char opt, char* error, size_t error_size) {
    return new_ir(name, opt, NULL, 1, error, error_size);
}

SnxIr* snx_ir_new_target(const char* name, char opt, const char* target_triple, char* error, size_t error_size) {
    return new_ir(name, opt, target_triple, 0, error, error_size);
}

static SnxIr* new_ir(const char* name, char opt, const char* target_triple, int jit, char* error, size_t error_size) {
    char *cpu, *features, *message = NULL;
    LLVMTargetRef target;
    LLVMTypeRef params[2];
    SnxIr* ir = (SnxIr*)calloc(1, sizeof(*ir));
    if (!ir) return NULL;
    ir->promote = opt != '0';
    const char* stats = getenv("SNX_IR_STATS");
    ir->stats.enabled = stats && *stats && strcmp(stats, "0");
    const char* dump = getenv("SNX_IR_DUMP_PC");
    if (dump && *dump) {
        char* end;
        ir->dump_pc = strtoull(dump, &end, 16);
        ir->dump_function = !*end;
    }
    char* triple = target_triple ? LLVMCreateMessage(target_triple) : host_triple(jit);
    if (LLVMGetTargetFromTriple(triple, &target, &message)) {
        snprintf(error, error_size, "LLVM target %s: %s", triple, message ? message : "unavailable");
        LLVMDisposeMessage(message);
        LLVMDisposeMessage(triple);
        free(ir);
        return NULL;
    }
    /* -march=native -mtune=native, -fPIC (toolchain.c). Another triple gets its architecture's baseline: x86-64 with
       CMPXCHG16B as rtlib is built, POWER8 on PowerPC64, rv64gc and LA64 below, anything else (AArch64) the generic CPU,
       since an x86 CPU name crashes another target's instruction selection */
    int ppc = !strncmp(triple, "powerpc64", 9) || !strncmp(triple, "ppc64", 5);
    if (!target_triple) {
        cpu = LLVMGetHostCPUName();
        features = LLVMGetHostCPUFeatures();
    } else if (!strncmp(triple, "x86_64", 6)) {
        cpu = LLVMCreateMessage("x86-64");
        features = LLVMCreateMessage("+cx16");
    } else {
        cpu = LLVMCreateMessage(ppc ? SNX_PPC_CPU : "generic");
        features = LLVMCreateMessage("");
    }
    /* The CPU cmake/cross.cmake builds the runtime library for at least (POWER8 unless the build names an older
       big-endian machine), whose modules then call RtAPI.cas128 for the 16-byte compare-and-swap. The host's name is
       "generic" where /proc/cpuinfo has no POWER line, as under qemu-user */
    if (ppc && (!strcmp(cpu, "generic") || !strcmp(cpu, "ppc64") || !strcmp(cpu, "ppc64le") || !*cpu)) {
        LLVMDisposeMessage(cpu);
        cpu = LLVMCreateMessage(SNX_PPC_CPU);
    }
    /* RISC-V and LoongArch: rv64gc and LA64 with double floats at least, as the runtime is built, so a module's float
       ABI (lp64d) is the runtime's whatever the host reports (qemu-user may report nothing), and no linker relaxation:
       link_elf.c moves no code, and refuses the ALIGN padding relaxation would have it take out */
    if (riscv_triple(triple) || loongarch_triple(triple)) {
        const char* base = riscv_triple(triple) ? "+m,+a,+f,+d,+c" : "+64bit,+f,+d";
        char* all = (char*)malloc(strlen(base) + strlen(features) + sizeof(",,-relax"));
        if (all) {
            sprintf(all, "%s,%s%s-relax", base, features, *features ? "," : "");
            LLVMDisposeMessage(features);
            features = LLVMCreateMessage(all);
            free(all);
        }
        if (!*cpu || !strcmp(cpu, "generic")) {
            LLVMDisposeMessage(cpu);
            cpu = LLVMCreateMessage(riscv_triple(triple) ? "generic-rv64" : "generic-la64");
        }
    }
    /* PowerPC64's ABI. The host's code is the runtime's own, which a triple doesn't settle: big-endian glibc is ELFv1
       on Debian's ports and ELFv2 on Arch POWER, and their LLVMs pick each for powerpc64-*-linux-gnu. Another
       triple's is what Clang picks for it: ELFv2 for little-endian and musl, ELFv1 for big-endian glibc */
    if (ppc) {
        LLVMTargetMachineOptionsRef options = LLVMCreateTargetMachineOptions();
#if defined(_CALL_ELF) && _CALL_ELF == 2
        ir->elfv2 = !target_triple;
#endif
        if (target_triple)
            ir->elfv2 = !strncmp(triple, "powerpc64le", 11) || !strncmp(triple, "ppc64le", 7) ||
                        strstr(triple, "musl") != NULL;
        LLVMTargetMachineOptionsSetCPU(options, cpu);
        LLVMTargetMachineOptionsSetFeatures(options, features);
        LLVMTargetMachineOptionsSetABI(options, ir->elfv2 ? "elfv2" : "elfv1");
        LLVMTargetMachineOptionsSetCodeGenOptLevel(options, codegen_level(opt));
        LLVMTargetMachineOptionsSetRelocMode(options, LLVMRelocPIC);
        LLVMTargetMachineOptionsSetCodeModel(options, LLVMCodeModelDefault);
        ir->tm = LLVMCreateTargetMachineWithOptions(target, triple, options);
        LLVMDisposeTargetMachineOptions(options);
    } else {
        /* LoongArch's calls as PCADDU18I and JIRL (CALL36, 128 GiB either way), which the medium code model makes: the
           small one's BL (B26) reaches 128 MiB, less than a game's code, and is LLVM 19's default (later ones make
           medium the default) */
        ir->tm = LLVMCreateTargetMachine(target, triple, cpu, features, codegen_level(opt), LLVMRelocPIC,
                                         loongarch_triple(triple) ? LLVMCodeModelMedium : LLVMCodeModelDefault);
    }
#ifdef __ANDROID__
    /* AArch64's machine outliner holds a unit's functions in memory and outlines nothing at O2: on Android that memory
       runs the recompile out of mappings */
    LLVMSetTargetMachineMachineOutliner(ir->tm, 0);
#endif
    ir->windows = strstr(triple, "windows") != NULL || strstr(triple, "mingw") != NULL;
    ir->features = target_triple ? &generic_features : &snx_ir_host;
    ir->ctx = LLVMContextCreate();
    ir->mod = LLVMModuleCreateWithNameInContext(name, ir->ctx);
    LLVMSetTarget(ir->mod, triple);
    LLVMTargetDataRef data = LLVMCreateTargetDataLayout(ir->tm);
    ir->big_endian = LLVMByteOrder(data) == LLVMBigEndian;
    char* layout = LLVMCopyStringRepOfTargetData(data);
    LLVMSetDataLayout(ir->mod, layout);
    LLVMDisposeMessage(layout);
    LLVMDisposeTargetData(data);
    LLVMDisposeMessage(triple);
    LLVMDisposeMessage(cpu);
    LLVMDisposeMessage(features);
    ir->b = LLVMCreateBuilderInContext(ir->ctx);
    ir->t_void = LLVMVoidTypeInContext(ir->ctx);
    ir->t_i1 = LLVMInt1TypeInContext(ir->ctx);
    ir->t_i8 = LLVMInt8TypeInContext(ir->ctx);
    ir->t_i16 = LLVMInt16TypeInContext(ir->ctx);
    ir->t_i32 = LLVMInt32TypeInContext(ir->ctx);
    ir->t_i64 = LLVMInt64TypeInContext(ir->ctx);
    ir->t_i128 = LLVMInt128TypeInContext(ir->ctx);
    ir->t_ptr = LLVMPointerTypeInContext(ir->ctx, 0);
    params[0] = ir->t_ptr;
    params[1] = ir->t_i64;
    ir->t_guest = LLVMFunctionType(ir->t_void, params, 2, 0);
    {
        /* !alias.scope/!noalias: one domain, two scopes, uniqued by name */
        LLVMMetadataRef cpu[2], guest[2];
        LLVMMetadataRef name = LLVMMDStringInContext2(ir->ctx, "snx.domain", 10);
        LLVMMetadataRef domain = LLVMMDNodeInContext2(ir->ctx, &name, 1);
        cpu[0] = LLVMMDStringInContext2(ir->ctx, "snx.cpu", 7);
        cpu[1] = domain;
        guest[0] = LLVMMDStringInContext2(ir->ctx, "snx.guest", 9);
        guest[1] = domain;
        LLVMMetadataRef list = LLVMMDNodeInContext2(ir->ctx, cpu, 2);
        ir->scope_cpu = LLVMMetadataAsValue(ir->ctx, LLVMMDNodeInContext2(ir->ctx, &list, 1));
        list = LLVMMDNodeInContext2(ir->ctx, guest, 2);
        ir->scope_guest = LLVMMetadataAsValue(ir->ctx, LLVMMDNodeInContext2(ir->ctx, &list, 1));
        ir->kind_scope = LLVMGetMDKindIDInContext(ir->ctx, "alias.scope", 11);
        ir->kind_noalias = LLVMGetMDKindIDInContext(ir->ctx, "noalias", 7);
    }
    return ir;
}

void snx_ir_free(SnxIr* ir) {
    if (!ir) return;
    LLVMDisposeBuilder(ir->b);
    LLVMDisposeModule(ir->mod);
    LLVMContextDispose(ir->ctx);
    LLVMDisposeTargetMachine(ir->tm);
    free(ir);
}

/* ---- PowerPC64's guest tail calls ----
   Without PC-relative code (POWER10), LLVM only makes a call a jump when it can see that the callee shares the caller's
   TOC: a function defined in the same module, never a declaration (another unit's guest function) or a pointer
   (snx_lookup's). For those a musttail call is a fatal error. A module has one TOC (link_elf.c), so the jump is always
   safe: each such call is made to a naked stub of this module, a definition LLVM jumps to, and the stub jumps on
   without touching r2 or the arguments. A declared callee's stub is a branch to it, which the linker points at its
   local entry (ELFv2) or code (ELFv1's descriptor); a pointer's stub repeats snx_lookup (cpu.h) and jumps through the
   count register, with r12 at the global entry as ELFv2 expects, or through the descriptor (ELFv1) */
static int ppc_target(const SnxIr* ir) {
    const char* target = LLVMGetTarget(ir->mod);
    return !strncmp(target, "powerpc64", 9) || !strncmp(target, "ppc64", 5);
}
static LLVMValueRef ppc_stub(SnxIr* ir, LLVMTypeRef type, const char* name, const char* text) {
    LLVMValueRef stub = LLVMGetNamedFunction(ir->mod, name);
    if (stub) return stub;
    stub = LLVMAddFunction(ir->mod, name, type);
    LLVMSetLinkage(stub, LLVMInternalLinkage);
    ir_attribute(ir, stub, LLVMAttributeFunctionIndex, "naked");
    ir_attribute(ir, stub, LLVMAttributeFunctionIndex, "noinline");
    ir_attribute(ir, stub, LLVMAttributeFunctionIndex, "nounwind");
    LLVMBuilderRef b = LLVMCreateBuilderInContext(ir->ctx);
    LLVMPositionBuilderAtEnd(b, LLVMAppendBasicBlockInContext(ir->ctx, stub, "entry"));
    LLVMValueRef body = LLVMGetInlineAsm(LLVMFunctionType(ir->t_void, NULL, 0, 0), text, strlen(text), "", 0, 1, 0,
                                         LLVMInlineAsmDialectATT, 0);
    LLVMBuildCall2(b, LLVMFunctionType(ir->t_void, NULL, 0, 0), body, NULL, 0, "");
    LLVMBuildUnreachable(b);
    LLVMDisposeBuilder(b);
    return stub;
}
static LLVMValueRef ppc_dynamic_stub(SnxIr* ir, LLVMTypeRef type) {
    char text[640];
    /* the module's ABI (new_ir): in ELFv1 a function pointer is its descriptor, whose first doubleword is the code */
    int v2 = ir->elfv2;
    const char* jump = v2 ? "mtctr 12\n bctr" : "ld 0, 0(12)\n ld 2, 8(12)\n mtctr 0\n bctr";
    const char* call = v2 ? "mtctr 12\n bctrl" : "ld 0, 0(12)\n ld 2, 8(12)\n mtctr 0\n bctrl";
    /* the ABI's TOC save doubleword of a frame: 24(r1) in ELFv2, 40(r1) in ELFv1 */
    unsigned toc_save = v2 ? 24 : 40;
    /* r3 the CPU, r4 the pc: f = disp[(pc - code_lo) >> 2] when inside and aligned, jumped to. Else c->rt->miss, the
       runtime's, with a TOC of its own: it is called, not jumped to, from a frame of the stub's (ELFv1's minimum, 112
       bytes), the link register saved in the caller's frame as a callee may and r2 in the stub's own frame, so the
       guest's caller gets the module's r2 back. Not in the caller's TOC save doubleword: the stub was jumped to, so
       r1 may still be the frame of the runtime's call into the module, whose saved TOC the runtime reloads after it
       (stored there, the module's TOC came back to the runtime in place of its own) */
    sprintf(text,
            "ld 11, %u(3)\n ld 12, %u(3)\n subf 12, 11, 12\n subf 11, 11, 4\n cmpld 11, 12\n bge 1f\n"
            " andi. 0, 4, 3\n bne 1f\n ld 12, %u(3)\n sldi 11, 11, 1\n ldx 12, 12, 11\n cmpdi 12, 0\n beq 1f\n"
            " %s\n1:\n mflr 0\n std 0, 16(1)\n stdu 1, -112(1)\n std 2, %u(1)\n ld 12, %u(3)\n ld 12, %u(12)\n"
            " %s\n ld 2, %u(1)\n addi 1, 1, 112\n ld 0, 16(1)\n mtlr 0\n blr",
            (unsigned)offsetof(CPU, code_lo), (unsigned)offsetof(CPU, code_hi), (unsigned)offsetof(CPU, disp), jump,
            toc_save, (unsigned)offsetof(CPU, rt), (unsigned)offsetof(RtAPI, miss), call, toc_save);
    return ppc_stub(ir, type, "snx_ppc_tail_dynamic", text);
}
static void ppc_tail_calls(SnxIr* ir) {
    LLVMValueRef next;
    for (LLVMValueRef function = LLVMGetFirstFunction(ir->mod); function; function = LLVMGetNextFunction(function))
        for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(function); block; block = LLVMGetNextBasicBlock(block))
            for (LLVMValueRef inst = LLVMGetFirstInstruction(block); inst; inst = next) {
                LLVMValueRef stub, args[2];
                char name[160], text[160];
                unsigned nargs, i;
                next = LLVMGetNextInstruction(inst);
                if (!LLVMIsACallInst(inst) || LLVMGetTailCallKind(inst) != LLVMTailCallKindMustTail) continue;
                LLVMValueRef callee = LLVMGetCalledValue(inst);
                LLVMTypeRef type = LLVMGetCalledFunctionType(inst);
                if (LLVMIsAFunction(callee) && !LLVMIsDeclaration(callee)) continue; /* LLVM jumps there itself */
                /* the musttail calls: a guest call (c, pc), to a declaration or a pointer, and an AArch32 block's
                   going on to the next (c, module_arm.c's chain_block), to a declaration. A pointer's stub is
                   snx_lookup's, for (c, pc) alone */
                nargs = LLVMGetNumArgOperands(inst);
                if (nargs > 2 || (!LLVMIsAFunction(callee) && nargs != 2)) continue;
                if (LLVMIsAFunction(callee)) {
                    size_t length;
                    const char* target = LLVMGetValueName2(callee, &length);
                    if (length > 120) continue;
                    sprintf(name, "snx_ppc_tail.%.*s", (int)length, target);
                    sprintf(text, "b %.*s", (int)length, target);
                    stub = ppc_stub(ir, type, name, text);
                } else
                    stub = ppc_dynamic_stub(ir, type);
                for (i = 0; i < nargs; ++i) args[i] = LLVMGetOperand(inst, i);
                LLVMPositionBuilderBefore(ir->b, inst);
                LLVMValueRef site = LLVMBuildCall2(ir->b, type, stub, args, nargs, "");
                LLVMSetTailCallKind(site, LLVMTailCallKindMustTail);
                LLVMInstructionEraseFromParent(inst);
            }
}

int snx_ir_object(SnxIr* ir, char opt, unsigned char** data, size_t* size, char* error, size_t error_size) {
    *data = NULL;
    *size = 0;
    if (ir->fallbacks) {
        SnxA64Insn insn;
        SnxA64Op op = snx_a64_decode_word(ir->fallback_word, ir->guest_features, &insn);
        snprintf(error, error_size, "no IR emitter for %s (%08lx at %llx), %lu instruction%s in all",
                 snx_a64_op_text(op), (unsigned long)ir->fallback_word, (unsigned long long)ir->fallback_pc,
                 ir->fallbacks, ir->fallbacks == 1 ? "" : "s");
        return -1;
    }
    return snx_ir_module_object(ir, ir->mod, opt, data, size, error, error_size);
}

void snx_ir_tune(SnxIr* ir, const SnxIrTuning* tuning) {
    ir->tuning = tuning;
    if (tuning && tuning->machine) tuning->machine(ir->tm);
}

/* -Os and -Oz as LLVM 23 has them, which refuses default<Os> and default<Oz> ("Use O2 in conjunction with the optsize
   attribute"): O2 over functions marked optsize, and minsize too for z, as Clang marks them. A function with optnone
   (none of ours) is left alone, as the verifier refuses both on one */
static void size_attributes(LLVMModuleRef module, char opt) {
    LLVMContextRef ctx = LLVMGetModuleContext(module);
    unsigned optsize = LLVMGetEnumAttributeKindForName("optsize", 7);
    unsigned minsize = LLVMGetEnumAttributeKindForName("minsize", 7);
    unsigned optnone = LLVMGetEnumAttributeKindForName("optnone", 7);
    for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn; fn = LLVMGetNextFunction(fn)) {
        if (LLVMIsDeclaration(fn) || LLVMGetEnumAttributeAtIndex(fn, LLVMAttributeFunctionIndex, optnone)) continue;
        LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex, LLVMCreateEnumAttribute(ctx, optsize, 0));
        if (opt == 'z')
            LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex, LLVMCreateEnumAttribute(ctx, minsize, 0));
    }
}

/* a change here, or to new_ir's target machine, that changes the object of the same IR bumps SNX_IR_OBJECT_VERSION */
int snx_ir_module_object(SnxIr* ir, LLVMModuleRef module, char opt, unsigned char** data, size_t* size, char* error,
                         size_t error_size) {
    char pipeline[16], *message = NULL;
    LLVMMemoryBufferRef buffer;
    *data = NULL;
    *size = 0;
    if (LLVMVerifyModule(module, LLVMReturnStatusAction, &message)) {
        snprintf(error, error_size, "invalid IR: %.900s", message ? message : "");
        LLVMDisposeMessage(message);
        return -1;
    }
    LLVMDisposeMessage(message);
    if (opt == 's' || opt == 'z') size_attributes(module, opt);
    sprintf(pipeline, "default<O%c>", opt == 's' || opt == 'z' ? '2' : opt);
    LLVMPassBuilderOptionsRef options = LLVMCreatePassBuilderOptions();
    if (ir->tuning && ir->tuning->builder) ir->tuning->builder(options);
    LLVMErrorRef failure =
        LLVMRunPasses(module, ir->tuning && ir->tuning->pipeline ? ir->tuning->pipeline : pipeline, ir->tm, options);
    LLVMDisposePassBuilderOptions(options);
    if (failure) {
        message = LLVMGetErrorMessage(failure);
        snprintf(error, error_size, "LLVM passes: %s", message);
        LLVMDisposeErrorMessage(message);
        return -1;
    }
    /* the NZCV values and other state a function computes in a loop only for the cold paths that store it (the
       poll's, a helper's call) are left in the loop by default<O?>, whose sinking takes an instruction only into a
       block its own is the only predecessor of: sink takes them as far down as the block dominating their uses */
    if (opt != '0' && !(ir->tuning && ir->tuning->pipeline)) {
        options = LLVMCreatePassBuilderOptions();
        failure = LLVMRunPasses(module, "function(sink,simplifycfg)", ir->tm, options);
        LLVMDisposePassBuilderOptions(options);
        if (failure) {
            message = LLVMGetErrorMessage(failure);
            snprintf(error, error_size, "LLVM passes: %s", message);
            LLVMDisposeErrorMessage(message);
            return -1;
        }
    }
    if (ppc_target(ir)) {
        ppc_tail_calls(ir);
        /* the pointer's load the stub repeats is dead now */
        options = LLVMCreatePassBuilderOptions();
        failure = LLVMRunPasses(ir->mod, "function(adce)", ir->tm, options);
        LLVMDisposePassBuilderOptions(options);
        if (failure) {
            message = LLVMGetErrorMessage(failure);
            snprintf(error, error_size, "LLVM passes: %s", message);
            LLVMDisposeErrorMessage(message);
            return -1;
        }
    }
    message = NULL;
    if (LLVMTargetMachineEmitToMemoryBuffer(ir->tm, module, LLVMObjectFile, &message, &buffer)) {
        snprintf(error, error_size, "LLVM code generation: %s", message ? message : "");
        LLVMDisposeMessage(message);
        return -1;
    }
    *size = LLVMGetBufferSize(buffer);
    *data = (unsigned char*)malloc(*size ? *size : 1);
    if (*data) memcpy(*data, LLVMGetBufferStart(buffer), *size);
    LLVMDisposeMemoryBuffer(buffer);
    if (!*data) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    return 0;
}

/* rtlib.c and armlib.c as LLVM bitcode (CMakeLists.txt, snx-rtlib-gen embed) */
extern const unsigned char snx_rtlib_bitcode[];
extern const unsigned long snx_rtlib_bitcode_size;
extern const unsigned char snx_armlib_bitcode[];
extern const unsigned long snx_armlib_bitcode_size;

static int library_object(const char* name, const unsigned char* bitcode, unsigned long bitcode_size, char opt,
                          unsigned char** data, size_t* size, char* error, size_t error_size) {
    static const char* const target_attributes[] = {"target-cpu", "target-features", "tune-cpu", "probe-stack"};
    LLVMModuleRef library;
    *data = NULL;
    *size = 0;
    SnxIr* ir = snx_ir_new(name, opt, error, error_size);
    if (!ir) return -1;
    /* The embedded bitcode has process lifetime. Parsing it needs a view, not
       another heap allocation and copy of the complete runtime library. */
    LLVMMemoryBufferRef buffer = LLVMCreateMemoryBufferWithMemoryRange((const char*)bitcode, bitcode_size, name, 0);
    int rc = LLVMParseBitcodeInContext2(ir->ctx, buffer, &library);
    LLVMDisposeMemoryBuffer(buffer);
    if (rc) {
        snprintf(error, error_size, "the runtime library's bitcode does not load into LLVM %s", LLVM_VERSION_STRING);
        snx_ir_free(ir);
        return -1;
    }
    /* Clang wrote the build machine's baseline CPU into every function, which would override the target machine's:
       without them the library is compiled for this CPU, as the generated code is.
       Apple's Clang also names __chkstk_darwin as the probe, which LLVM's AArch64 backend rejects */
    for (LLVMValueRef fn = LLVMGetFirstFunction(library); fn; fn = LLVMGetNextFunction(fn))
        for (unsigned i = 0; i < sizeof(target_attributes) / sizeof(*target_attributes); ++i)
            LLVMRemoveStringAttributeAtIndex(fn, LLVMAttributeFunctionIndex, target_attributes[i],
                                             (unsigned)strlen(target_attributes[i]));
    LLVMSetTarget(library, LLVMGetTarget(ir->mod));
    LLVMSetDataLayout(library, LLVMGetDataLayoutStr(ir->mod));
    LLVMDisposeModule(ir->mod);
    ir->mod = library;
    /* optimized by Clang already: code generation only */
    rc = snx_ir_object(ir, '0', data, size, error, error_size);
    snx_ir_free(ir);
    return rc;
}
int snx_ir_rtlib_object(char opt, unsigned char** data, size_t* size, char* error, size_t error_size) {
    return library_object("rtlib", snx_rtlib_bitcode, snx_rtlib_bitcode_size, opt, data, size, error, error_size);
}
int snx_ir_armlib_object(char opt, unsigned char** data, size_t* size, char* error, size_t error_size) {
    return library_object("armlib", snx_armlib_bitcode, snx_armlib_bitcode_size, opt, data, size, error, error_size);
}

char* snx_ir_text(SnxIr* ir) { return LLVMPrintModuleToString(ir->mod); }
