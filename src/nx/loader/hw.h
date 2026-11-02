/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The console a program is built for, as its NPDM says (docs/SWITCH2.md). The recompiler compiles the program's code
   for that console's CPU (codegen_view.h's snx_hw_a64), and the runtime gives the program that console's kernel. Both
   read it from the same NPDM through the same function here, so the AOT code and the runtime JIT decode alike */
#ifndef SNX_NX_LOADER_HW_H
#define SNX_NX_LOADER_HW_H

#include "common/utilities/endian.h"
#include <stddef.h>
#include <string.h>

typedef enum { SNX_HW_SWITCH, SNX_HW_SWITCH2 } SnxHw;

/* The console of an NPDM, size bytes at data. A Switch 2 NPDM's ACID has a 0x180-byte signature and key, where a
   Switch 1 one has 0x100-byte ones, so its magic is at 0x300 and not at 0x200 (Switchbrew's NPDM page, and eden-ns2's
   "extended ACID"). An NPDM that is too short or has no META is a Switch 1 program's, as a program without one is */
static inline SnxHw snx_npdm_hw(const unsigned char* data, size_t size) {
    if (!data || size < 0x80 || memcmp(data, "META", 4)) return SNX_HW_SWITCH;
    uint32_t acid = snx_ld_le32(data + 0x78), acid_size = snx_ld_le32(data + 0x7C);
    if (acid > size || acid_size > size - acid || acid_size < 0x304) return SNX_HW_SWITCH;
    return memcmp(data + acid + 0x300, "ACID", 4) ? SNX_HW_SWITCH : SNX_HW_SWITCH2;
}

#endif
