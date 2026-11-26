/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A64 disassembly for scarletnx --mcp (disasm.c): each word as the A64 decoder has it, a64.decode's op name and its
   arguments */
#ifndef SNX_DISASM_H
#define SNX_DISASM_H
#include "common/utilities/common.h"

/* one instruction: its address and word, and its text */
typedef void (*SnxDisasmLine)(void* ctx, SnxU64 address, SnxU32 word, const char* text);

/* each 4-byte word of size bytes of code at guest address, in order, as the A64 decoder has it
   (snx_a64_word_fields: "ADD_i rd=1 rn=2 imm=291 sf=1"), "<unknown>" for a word it doesn't decode */
void snx_disasm_a64(const unsigned char* code, size_t size, SnxU64 address, SnxDisasmLine line, void* ctx);
/* the address the PC-relative branch or ADR word at pc names, 0 for any other instruction */
SnxU64 snx_disasm_a64_target(SnxU32 word, SnxU64 pc);

#endif
