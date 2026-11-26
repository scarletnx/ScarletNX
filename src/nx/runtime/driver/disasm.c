/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_disasm_a64 (disasm.h): the A64 decoder's text of each word (a64_decoder.h), and the targets of the PC-relative
   instructions decoded from their encoding (the Arm ARM's B, BL, B.cond, CBZ, CBNZ, TBZ, TBNZ and ADR), which mcp.c
   names */
#include "nx/runtime/driver/disasm.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"

SnxU64 snx_disasm_a64_target(SnxU32 w, SnxU64 pc) {
    if ((w & 0x7c000000u) == 0x14000000u) return pc + (SnxU64)(snx_sext64(w, 26) * 4);      /* B, BL */
    if ((w & 0xff000010u) == 0x54000000u) return pc + (SnxU64)(snx_sext64(w >> 5, 19) * 4); /* B.cond */
    if ((w & 0x7e000000u) == 0x34000000u) return pc + (SnxU64)(snx_sext64(w >> 5, 19) * 4); /* CBZ, CBNZ */
    if ((w & 0x7e000000u) == 0x36000000u) return pc + (SnxU64)(snx_sext64(w >> 5, 14) * 4); /* TBZ, TBNZ */
    if ((w & 0x9f000000u) == 0x10000000u)                                                   /* ADR */
        return pc + (SnxU64)snx_sext64(((w >> 5) & 0x7ffffu) << 2 | ((w >> 29) & 3u), 21);
    return 0;
}

void snx_disasm_a64(const unsigned char* code, size_t size, SnxU64 address, SnxDisasmLine line, void* ctx) {
    char text[256];
    for (size_t i = 0; i + 4 <= size; i += 4) {
        SnxU32 w = snx_ld_le32(code + i);
        line(ctx, address + i, w, snx_a64_word_fields(w, text, sizeof text) ? text : "<unknown>");
    }
}
