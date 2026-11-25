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
/* 1 where this host runs the powerpc64 back end's code and its objects load (big-endian PowerPC64, ELFv2, LDBRX) */
int snx_ppc64be_available(void);
/* 1 on a big-endian PowerPC64 host the code doesn't run on, with the line of why in why (snx_native_floor): an ELFv1
   program, or a processor without LDBRX and STDBRX (before ISA 2.06, but the Cell); 0 and "" elsewhere */
int snx_ppc64be_floor(char* why, size_t size);
/* snx_ppc64be_floor of a host whose AT_HWCAP is hwcap and AT_PLATFORM platform (NULL for none), of the ELF ABI elf */
int snx_ppc64be_floor_of(unsigned long hwcap, const char* platform, int elf, char* why, size_t size);
/* what is taken out of the host's AT_HWCAP from here on, for a test of a host without it (0 none) */
void snx_ppc64be_without(unsigned long hwcap);
/* the object of the functions [first, last) of codegen it can make, made[i - first] 1 for each; for a JIT unit all
   of them or none. The object's size, or 0 with none made, -1 with the reason in error */
int snx_ppc64be_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                    unsigned char* made, unsigned char** object, size_t* size, char* error, size_t error_size);
#endif
