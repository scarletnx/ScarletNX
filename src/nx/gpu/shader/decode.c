/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Maxwell instruction decoder. The encodings are sm53.decode; tools/decodetree turns them into a switch on the top 16
   bits of the word (decode-sm53.inc in the build folder), whose trans_ functions name the opcode, so nothing is built
   at run time. */
#include "nx/gpu/shader/translator.h"
#include "common/utilities/decodetree.h"

/* the decoder's context: the opcode the trans_ function of the matching pattern names */
typedef const Op* DisasContext;
#include "decode-sm53.inc"

static const Op ops[O__COUNT] = {
    {O_NONE, "(unknown)"},
#define SM53_ROW(name, show) {O_##name, show},
    SM53_OPS(SM53_ROW)
#undef SM53_ROW
};
/* CCTL's further encoding, under a name of its own */
static const Op cctlt = {O_CCTL, "CCTLT"};

#define SM53_TRANS(name, show)                                                                                         \
    static bool trans_##name(DisasContext* ctx, arg_##name* a) {                                                       \
        (void)a;                                                                                                       \
        *ctx = &ops[O_##name];                                                                                         \
        return true;                                                                                                   \
    }
SM53_OPS(SM53_TRANS)
#undef SM53_TRANS
static bool trans_CCTLT(DisasContext* ctx, arg_CCTLT* a) {
    (void)a;
    *ctx = &cctlt;
    return true;
}

const Op* decode(u64 w) {
    const Op* op = NULL;
    if (!decode_sm53(&op, (uint16_t)(w >> 48))) return NULL;
    /* EXIT.FCSM_TR (flow condition 0x1C), which the compiler puts after VOTE.VTG, is the hardware's cull exit: as an
       EXIT the vertex program would end before writing its position */
    if (op->id == O_EXIT && B(w, 0, 5) == 0x1C) return &ops[O_NOP];
    return op;
}
