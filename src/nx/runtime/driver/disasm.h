/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A64 disassembly through LLVM's disassembler (disasm.c, GNU C23 for the LLVM-C headers), for scarletnx --mcp. It is built
   where LLVM has its AArch64 target (SNX_A64_DISASM, cmake/llvm.cmake): elsewhere snx_disasm_a64 says it has none */
#ifndef SNX_DISASM_H
#define SNX_DISASM_H
#include "common/utilities/common.h"

/* one instruction: its address and word, its text, and the address a branch or ADR names (0 for none) */
typedef void (*SnxDisasmLine)(void* ctx, SnxU64 address, SnxU32 word, const char* text, SnxU64 target);

/* each 4-byte word of size bytes of code at guest address, in order ("<unknown>" for a word LLVM doesn't decode).
   0, or -1 when this build has no A64 disassembler */
int snx_disasm_a64(const unsigned char* code, size_t size, SnxU64 address, SnxDisasmLine line, void* ctx);

#endif
