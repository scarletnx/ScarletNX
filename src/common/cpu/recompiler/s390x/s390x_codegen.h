/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The s390x back end's guest functions (s390x_codegen.c): the recompiler's code without LLVM on an s390x host, from
   the A64 emitters sailgen generates with its s390x back end (tools/sailgen/s390x.h and risc.h, its instructions
   LLVM's SystemZ TableGen records'), written as an ELF s390x object the linker and the JIT's loader take as they take
   LLVM's. It builds on any host (the code is s390x's: a test runs it under Unicorn); a function it can't make is left
   to another back end */
#ifndef SNX_RECOMPILER_S390X_CODEGEN_H
#define SNX_RECOMPILER_S390X_CODEGEN_H
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/mach/native.h"
#include <stddef.h>
/* 1 where this host runs the s390x back end's code and its objects load (s390x, ELF) */
int snx_s390x_available(void);
/* the object of the functions [first, last) of codegen it can make, made[i - first] 1 for each; for a JIT unit all
   of them or none. The object's size, or 0 with none made, -1 with the reason in error */
int snx_s390x_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                    unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size);
#endif
