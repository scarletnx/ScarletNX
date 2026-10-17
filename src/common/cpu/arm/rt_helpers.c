/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/translate.c, vec_helper.c, vfp_helper.c, crypto_helper.c),
   LGPL-2.1-or-later:
   Copyright (c) 2003 Fabrice Bellard
   Copyright (c) 2005-2007 CodeSourcery
   Copyright (c) 2007 OpenedHand, Ltd.
   Copyright (c) 2018 Linaro
   Copyright (C) 2013 - 2018 Linaro Ltd <ard.biesheuvel@linaro.org> */
/* The runtime library's calls into the backend (arm.h's SnxArmRt): what generated code can't do itself is decided by
   the runtime running it, as A64 code calls RtAPI's undef. Linked into snx_arm_rt and every 32-bit game module */
#include "common/cpu/arm/arm.h"

void snx_arm_undefined(SnxArmCPU* cpu, uint32_t word, uint32_t reason) {
    if (cpu->rt && cpu->rt->undefined) {
        cpu->rt->undefined(cpu, word, reason);
        return;
    }
    cpu->exit = reason;
    cpu->syndrome = word;
}

void snx_arm_coproc(SnxArmCPU* cpu, uint32_t word) {
    if (cpu->rt && cpu->rt->coproc)
        cpu->rt->coproc(cpu, word);
    else
        snx_arm_coproc_user(cpu, word);
}

void snx_arm_coproc_user(SnxArmCPU* cpu, uint32_t word) {
    /* MRC/MCR p15, 0, Rt, c13, c0, 2 (TPIDRURW) or 3 (TPIDRURO, read only) */
    unsigned rt = (word >> 12) & 15, load = (word >> 20) & 1, opc1 = (word >> 21) & 7, crn = (word >> 16) & 15;
    unsigned coproc = (word >> 8) & 15, opc2 = (word >> 5) & 7, crm = word & 15;
    if ((word & 0x0F000010) == 0x0E000010 && coproc == 15 && !opc1 && crn == 13 && !crm && rt != 15 &&
        (opc2 == 2 || (opc2 == 3 && load))) {
        if (!load)
            cpu->tpidrurw = cpu->r[rt];
        else
            cpu->r[rt] = opc2 == 3 ? cpu->tpidruro : cpu->tpidrurw;
        return;
    }
    cpu->exit = SNX_ARM_UNDEFINED;
    cpu->syndrome = word;
}
