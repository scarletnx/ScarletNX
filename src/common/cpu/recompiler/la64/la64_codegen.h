/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The loongarch64 back end's guest functions (la64_codegen.c): the recompiler's code without LLVM on a loongarch64 host, from
   the A64 emitters sailgen generates with its loongarch64 back end (tools/sailgen/la64.h and risc.h, its words LLVM's
   LoongArch TableGen records'), written as an ELF LoongArch object the linker and the JIT's loader take as they take LLVM's.
   It builds on any host (the code is loongarch64's: a test runs it in tests/support/la64_interp.c); a function it can't make is left to
   another back end */
#ifndef SNX_RECOMPILER_LA64_CODEGEN_H
#define SNX_RECOMPILER_LA64_CODEGEN_H
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/mach/native.h"
#include <stddef.h>
/* 1 where this host runs the loongarch64 back end's code and its objects load (LoongArch64, ELF) */
int snx_la64_available(void);
/* the object of the functions [first, last) of codegen it can make, made[i - first] 1 for each; for a JIT unit all
   of them or none. The object's size, or 0 with none made, -1 with the reason in error */
int snx_la64_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                    unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size);
#endif
