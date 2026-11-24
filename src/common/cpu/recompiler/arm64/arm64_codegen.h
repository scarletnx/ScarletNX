/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The arm64 back end's guest functions (arm64_codegen.c): the recompiler's code without LLVM on an arm64 host, from
   the A64 emitters sailgen generates with its arm64 back end (tools/sailgen/arm64.h, its words the A64 Sail's
   encoders), written as an ELF AArch64 object the linker and the JIT's loader take as they take LLVM's. It builds on
   any host (the code is arm64's: a test runs it under Unicorn); a function it can't make is left to another back end */
#ifndef SNX_RECOMPILER_ARM64_CODEGEN_H
#define SNX_RECOMPILER_ARM64_CODEGEN_H
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/mach/native.h"
#include <stddef.h>
/* the host's features the back end's forms may need (sailgen's arm64.h SGX_F_*: LSE, CRC32, CSSC) */
enum { SNX_ARM64_F_LSE = 1, SNX_ARM64_F_CRC32 = 2, SNX_ARM64_F_CSSC = 4 };
/* 1 where this host runs the arm64 back end's code and its objects load (AArch64, ELF: not Windows or Apple's) */
int snx_arm64_available(void);
/* this host's features (0 where it isn't arm64) */
unsigned snx_arm64_features(void);
/* the object of the functions [first, last) of codegen it can make, made[i - first] 1 for each, for a host of these
   features (snx_arm64_features() for this one's); for a JIT unit all of them or none. The object's size, or 0 with none
   made, -1 with the reason in error */
int snx_arm64_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options, unsigned host,
                     unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size);
#endif
