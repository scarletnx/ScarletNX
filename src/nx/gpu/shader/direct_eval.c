/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Executes the compacted Maxwell instructions that recover a texture handle from memory. The collector in dataflow.c
   accepts only the same unpredicated MOV_C, 32-bit LDG.E and plain LOP.OR/IADD forms. */
#include "nx/gpu/shader/translator.h"
#include "common/utilities/bits.h"

int direct_eval(const u64* insn, u32 n, const Sm86EvalIo* io, const u32* probes, u32 nprobes, u32* out) {
    for (u32 k = 0; k < nprobes; k++) out[k] = 0;
    if (!io || !io->cbuf || !io->mem) return 0;
    u32 r[256] = {0};
    for (u32 i = 0; i < n; i++) {
        u64 w = insn[i];
        const Op* op = decode(w);
        u32 d = B(w, 0, 8), a = B(w, 8, 8);
        if (!op || B(w, 16, 4) != 7) return 0;
        switch (op->id) {
            case O_MOV_C: {
                u32 form = B(w, 39, 4), bank = B(w, 34, 5), offset = B(w, 20, 14) * 4;
                if (d == 255 || (form != 1 && form != 15) || bank >= 18) return 0;
                r[d] = io->cbuf(io->user, bank, offset);
                break;
            }
            case O_LDG: {
                if (d == 255 || a >= 254 || B(w, 48, 3) != 4 || B(w, 45, 1) != 1) return 0;
                i32 displacement = (i32)(B(w, 20, 24) << 8) / 256;
                u64 address = (u64)r[a] | (u64)r[a + 1] << 32;
                address += (i64)displacement;
                if (!io->mem(io->user, address, &r[d], 1)) return 0;
                break;
            }
            case O_LOP_R: {
                u32 b = B(w, 20, 8);
                if (d == 255 || B(w, 41, 2) != 1 || B(w, 39, 2) != 0 || B(w, 47, 1) != 0) return 0;
                r[d] = r[a] | r[b];
                break;
            }
            case O_IADD_R: {
                u32 b = B(w, 20, 8);
                if (d == 255 || B(w, 43, 1) || B(w, 47, 1) || B(w, 48, 2)) return 0;
                r[d] = r[a] + r[b];
                break;
            }
            default: return 0;
        }
        for (u32 k = 0; k < nprobes; k++) {
            if (SH_PROBE_INSN(probes[k]) == i) {
                if (SH_PROBE_UNIFORM(probes[k])) return 0;
                out[k] = r[SH_PROBE_REG(probes[k])];
            }
        }
    }
    return 1;
}

int direct_eval_memh(const u64 (*memh)[2], u32 n, const Sm86EvalIo* io, const u32* probes, u32 nprobes,
                     u32* out) {
    u64 flat[SH_MEMH_MAX];
    if (n > SH_MEMH_MAX) return 0;
    for (u32 i = 0; i < n; i++) flat[i] = memh[i][0];
    return direct_eval(flat, n, io, probes, nprobes, out);
}
