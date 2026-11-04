/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The LLVM backend's step functions for the instruction tests: each word one function (snx_ir_static_step, the word's
   pc a constant in it), linked with the runtime library and loaded into this process, either as a module file for
   dlopen or LoadLibrary, as the AOT path loads a game's, or with the JIT's loader (code_loader.c), as the runtime
   JIT loads its units. The JIT's loader takes ELF objects on Linux and macOS and COFF ones on Windows, so a test that
   loads with it runs on every host the recompiler runs on */
#ifndef SNX_TEST_LLVM_STEPS_H
#define SNX_TEST_LLVM_STEPS_H

#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/llvm/code_loader.h"

typedef u64 (*LlvmStep)(CPU* c);

/* what of the host the emitters may use (llvm_steps_begin's portable) */
enum {
    LLVM_STEPS_HOST,     /* the host's architecture and extensions, as the recompiler builds for it */
    LLVM_STEPS_PORTABLE, /* the architecture without its extensions: the emitters' portable forms (x86's PSHUFB,
                            PCLMULQDQ, AES-NI, SSE4.2's CRC32C and the like, AArch64's CRC32 and crypto) */
    LLVM_STEPS_GENERIC   /* neither: the path of the hosts SnxIrFeatures has no bit for (PowerPC64, RISC-V,
                            LoongArch, MIPS64), as an SnxIr for another triple gets it */
};

typedef struct {
    int jit;      /* load with the JIT's loader, not as a module file */
    char opt;     /* both the target machine and the IR pipeline: normally '2' */
    int portable; /* LLVM_STEPS_HOST, LLVM_STEPS_PORTABLE or LLVM_STEPS_GENERIC */
    SnxIr* ir;    /* while steps are added; its functions in fns */
    LLVMValueRef* fns;
    size_t count, cap;
    LlvmStep* steps; /* once loaded: the step of each add, by index */
    SnxCodeLoader* loader;
    void* library;
    char* dir; /* the module file's */
    /* the emitters' (ir->features) unless portable is LLVM_STEPS_HOST */
    SnxIrFeatures features;
} LlvmSteps;

/* starts a set of steps (snx_ir_init included), portable one of the LLVM_STEPS_ values: 0, or -1 with the reason
   in error */
int llvm_steps_begin(LlvmSteps* s, int jit, int portable, char* error, size_t error_size);
int llvm_steps_begin_opt(LlvmSteps* s, int jit, int portable, char opt, char* error, size_t error_size);
/* word's step at pc for a guest with these A64 features (0: the Switch's); its index */
size_t llvm_steps_add(LlvmSteps* s, u32 word, u64 pc, u64 features);
/* function's code (snx_ir_emit), at its index in the table as its entry F_<address>: a GuestFn, not an LlvmStep */
size_t llvm_steps_add_function(LlvmSteps* s, SnxCodegen* codegen, const SnxFunction* function);
/* compiles and loads the steps added, s->steps then holding them. In JIT mode the loader is also checked to resolve
   symbols of this executable, as the runtime's are resolved for JIT code. 0, or -1 with the reason in error */
int llvm_steps_load(LlvmSteps* s, char* error, size_t error_size);
/* Saves the actual pre/post optimization IR and object. The caller must end these steps afterward. */
int llvm_steps_dump(LlvmSteps* s, const char* prefix, char* error, size_t error_size);
void llvm_steps_end(LlvmSteps* s);

#endif
