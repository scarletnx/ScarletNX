/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/translate.c, vec_helper.c, vfp_helper.c, crypto_helper.c),
   LGPL-2.1-or-later:
   Copyright (c) 2003 Fabrice Bellard
   Copyright (c) 2005-2007 CodeSourcery
   Copyright (c) 2007 OpenedHand, Ltd.
   Copyright (c) 2018 Linaro
   Copyright (C) 2013 - 2018 Linaro Ltd <ard.biesheuvel@linaro.org> */
#include "common/cpu/arm/simd.h"
/* The shared reference rounds retain their original notices. No AES-NI,
   CLMUL or host feature dispatch in this initial A32 implementation. */
#define SNX_CRYPTO_PORTABLE
#define v_aes    snx_arm_reference_aes
#define v_aesmc  snx_arm_reference_aesmc
#define v_sha1   snx_arm_reference_sha1
#define v_sha256 snx_arm_reference_sha256
#define v_pmull  snx_arm_reference_pmull
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/aarch64/helpers/crypto.c"

void snx_arm_crypto(SnxArmCPU* cpu, uint32_t d, uint32_t n, uint32_t m, uint32_t op) {
    CPU c;
    unsigned regs[3] = {d, n, m};
    for (unsigned r = 0; r < 3; ++r) {
        for (unsigned i = 0; i < (op < 4 ? 16u : 4u); ++i) {
            if (op < 4)
                c.v[r].b[i] = (u8)(cpu->d[regs[r] / 8 + i / 8] >> ((i % 8) * 8));
            else
                c.v[r].s[i] = (u32)(cpu->d[regs[r] / 8 + i / 2] >> ((i % 2) * 32));
        }
    }
    if (op < 2)
        v_aes(&c.v[0], &c.v[2], op);
    else if (op < 4)
        v_aesmc(&c.v[0], &c.v[2], op - 2);
    else if (op < 8)
        v_sha1(&c, op - 4, 0, 1, 2);
    else if (op < 11)
        v_sha256(&c, op - 4, 0, 1, 2);
    else if (op == 11 || op == 12)
        v_sha1(&c, op == 11 ? 0x10 : 0x11, 0, 2, 2);
    else
        v_sha256(&c, 0x12, 0, 2, 2);
    uint64_t lo = 0, hi = 0;
    if (op < 4) {
        for (unsigned i = 0; i < 8; ++i) {
            lo |= (u64)c.v[0].b[i] << (i * 8);
            hi |= (u64)c.v[0].b[i + 8] << (i * 8);
        }
    } else {
        lo = c.v[0].s[0] | ((u64)c.v[0].s[1] << 32);
        hi = c.v[0].s[2] | ((u64)c.v[0].s[3] << 32);
    }
    cpu->d[d / 8] = lo;
    cpu->d[d / 8 + 1] = hi;
}
