/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_disasm_a64 (disasm.h): LLVM's AArch64 disassembler through its C API, and the targets of the PC-relative
   instructions decoded from their encoding (the Arm ARM's B, BL, B.cond, CBZ, CBNZ, TBZ, TBNZ and ADR), which LLVM
   prints as offsets */
#include "nx/runtime/driver/disasm.h"
#include "common/utilities/bits.h"
#ifdef SNX_A64_DISASM
#include <llvm-c/Disassembler.h>
#include <llvm-c/Target.h>
#endif

/* the address a PC-relative branch or ADR at pc names, 0 for any other instruction */
static SnxU64 target(SnxU32 w, SnxU64 pc) {
    if ((w & 0x7c000000u) == 0x14000000u) return pc + (SnxU64)(snx_sext64(w, 26) * 4);      /* B, BL */
    if ((w & 0xff000010u) == 0x54000000u) return pc + (SnxU64)(snx_sext64(w >> 5, 19) * 4); /* B.cond */
    if ((w & 0x7e000000u) == 0x34000000u) return pc + (SnxU64)(snx_sext64(w >> 5, 19) * 4); /* CBZ, CBNZ */
    if ((w & 0x7e000000u) == 0x36000000u) return pc + (SnxU64)(snx_sext64(w >> 5, 14) * 4); /* TBZ, TBNZ */
    if ((w & 0x9f000000u) == 0x10000000u)                                             /* ADR */
        return pc + (SnxU64)snx_sext64(((w >> 5) & 0x7ffffu) << 2 | ((w >> 29) & 3u), 21);
    return 0;
}

int snx_disasm_a64(const unsigned char* code, size_t size, SnxU64 address, SnxDisasmLine line, void* ctx) {
#ifdef SNX_A64_DISASM
    LLVMDisasmContextRef dc;
    char text[256];
    size_t i;
    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64Disassembler();
    dc = LLVMCreateDisasm("aarch64-none-elf", NULL, 0, NULL, NULL);
    if (!dc) return -1;
    LLVMSetDisasmOptions(dc, LLVMDisassembler_Option_PrintImmHex);
    for (i = 0; i + 4 <= size; i += 4) {
        SnxU32 w = (SnxU32)code[i] | (SnxU32)code[i + 1] << 8 | (SnxU32)code[i + 2] << 16 | (SnxU32)code[i + 3] << 24;
        const char* t = text;
        if (!LLVMDisasmInstruction(dc, (uint8_t*)code + i, 4, address + i, text, sizeof text)) t = "<unknown>";
        while (*t == ' ' || *t == '\t') t++;
        line(ctx, address + i, w, t, target(w, address + i));
    }
    LLVMDisasmDispose(dc);
    return 0;
#else
    (void)code;
    (void)size;
    (void)address;
    (void)line;
    (void)ctx;
    (void)target;
    return -1;
#endif
}
