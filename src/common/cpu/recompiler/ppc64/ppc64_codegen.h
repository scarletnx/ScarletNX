/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The powerpc64 back end's guest functions (ppc64_codegen.c): the recompiler's code on a powerpc64 host, from the A64
   emitters sailgen generates with its powerpc64 back end (tools/sailgen/ppc64.h and risc.h, its words LLVM's PowerPC
   TableGen records'), written as an ELF PowerPC64 (ELFv2) object which the linker and the JIT's loader take. It builds
   on any host (the code is powerpc64's: a test runs it under Unicorn); a function it can't make whole is said */
#ifndef SNX_RECOMPILER_PPC64_CODEGEN_H
#define SNX_RECOMPILER_PPC64_CODEGEN_H
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/mach/native.h"
#include <stddef.h>
/* 1 where this host runs the powerpc64 back end's code and its objects load (little-endian PowerPC64, ELFv2) */
int snx_ppc64_available(void);
/* the object of the functions [first, last) of codegen it can make, made[i - first] 1 for each; for a JIT unit all
   of them or none. The object's size, or 0 with none made, -1 with the reason in error */
int snx_ppc64_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                    unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size);
#endif
