/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The arm64 back end's guest functions (arm64_codegen.c): the recompiler's code on an arm64 host, from the A64 emitters
   sailgen generates with its arm64 back end (tools/sailgen/arm64.h, its words the A64 Sail's encoders), written as an
   ELF AArch64 object, or on Windows a COFF one, which the linker and the JIT's loader take. It builds on any host (the
   code is arm64's: a test runs it under Unicorn); a function it can't make is left to another back end */
#ifndef SNX_RECOMPILER_ARM64_CODEGEN_H
#define SNX_RECOMPILER_ARM64_CODEGEN_H
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/mach/native.h"
#include <stddef.h>
/* the host's features the back end's forms may need (the first three sailgen's arm64.h SGX_F_*: LSE, CRC32, CSSC),
   and those a guest's Advanced SIMD or floating-point word may need to run as the host's own (arm64_vec.c) */
enum {
    SNX_ARM64_F_LSE = 1, SNX_ARM64_F_CRC32 = 2, SNX_ARM64_F_CSSC = 4, SNX_ARM64_F_AES = 8, SNX_ARM64_F_PMULL = 16,
    SNX_ARM64_F_SHA1 = 32, SNX_ARM64_F_SHA256 = 64, SNX_ARM64_F_RDM = 128, SNX_ARM64_F_FP16 = 256,
    SNX_ARM64_F_DOTPROD = 512, SNX_ARM64_F_FHM = 1024, SNX_ARM64_F_SHA512 = 2048, SNX_ARM64_F_SHA3 = 4096,
    SNX_ARM64_F_SM = 8192, SNX_ARM64_F_JSCVT = 16384, SNX_ARM64_F_FCMA = 32768, SNX_ARM64_F_FRINTTS = 65536,
    SNX_ARM64_F_BF16 = 1 << 17, SNX_ARM64_F_I8MM = 1 << 18,
    SNX_ARM64_F_WIN = 1 << 19 /* no processor's: the code is Windows', in COFF objects with a function table */
};
#define SNX_ARM64_F_ALL ((1u << 19) - 1)
/* how far past the state's address the code's X19 holds (sailgen's arm64.h, SGX_BASE_BIAS): a field at offset off of
   the state is at [X19, #off - SNX_ARM64_STATE_BIAS] */
#define SNX_ARM64_STATE_BIAS 8
/* 1 where this host runs the arm64 back end's code and its objects load: AArch64 whose system's objects are ELF or
   Windows' COFF, or Apple's, whose JIT loader and module linker take the back end's ELF objects (code_loader.c,
   link_macho.c) */
int snx_arm64_available(void);
/* this host's features (0 where it isn't arm64) */
unsigned snx_arm64_features(void);
/* the features (SNX_ARM64_F_*) the back end goes without from here on though the host has them, for a test of a host
   without them (0 none); SNX_ARM64_F_WIN, no processor's, stays */
void snx_arm64_without(unsigned features);
/* 1 where the code is made for Windows: on Windows, or where a test asked for it (snx_arm64_set_windows) */
int snx_arm64_windows(void);
/* the AArch32 blocks (arm64_arm.h) and the module's descriptor made from here on as a Windows host's (1), or as this
   host's own (0), for a test of them elsewhere; a Windows host makes its own alone. A function's object is by its
   host's features (SNX_ARM64_F_WIN) */
void snx_arm64_set_windows(int on);
/* the object of the functions [first, last) of codegen it can make, made[i - first] 1 for each, for a host of these
   features (snx_arm64_features() for this one's); for a JIT unit all of them or none. The object's size, or 0 with none
   made, -1 with the reason in error */
int snx_arm64_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options, unsigned host,
                     unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size);
#endif
