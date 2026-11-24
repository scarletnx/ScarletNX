/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The ppc64 back end's guest functions for a big-endian PowerPC64 (ELFv2) host: ppc64_codegen.h's
   functions, the same back end's with PPC_BE 1 (ppc64be_codegen.c): its words, its guest memory's accesses and
   its objects in that order */
#ifndef SNX_RECOMPILER_PPC64BE_CODEGEN_H
#define SNX_RECOMPILER_PPC64BE_CODEGEN_H
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/mach/native.h"
#include <stddef.h>
/* 1 where this host runs the powerpc64 back end's code and its objects load (little-endian PowerPC64, ELFv2) */
int snx_ppc64be_available(void);
/* the object of the functions [first, last) of codegen it can make, made[i - first] 1 for each; for a JIT unit all
   of them or none. The object's size, or 0 with none made, -1 with the reason in error */
int snx_ppc64be_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                    unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size);
#endif
