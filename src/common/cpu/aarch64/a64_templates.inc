/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/translate-a64.c, helper-a64.c, vec_helper.c, vfp_helper.c,
   crypto_helper.c), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de>
   Copyright (c) 2018 Linaro
   Copyright (C) 2013 - 2018 Linaro Ltd <ard.biesheuvel@linaro.org>
   Copyright (c) 2003 Fabrice Bellard

   Maintained C source: decoder patterns and specialized instruction templates.
   No generator is needed to build.
   Each row: flow kind, field declarations, C body/operand, optional branch target.
   Constants w and pc specialize these same templates for static AOT.
   Edit instruction semantics here for both AOT and interpreter emission. */

/* ADR: specialized by no fields. */
static const SnxA64Case cases_ADR[] = {
    { SNX_FLOW_PLAIN,
      "const i64 f_immhi=(i64)((i32)(w<<8)>>13); const u32 f_immlo=(w>>29)&3; const u32 f_rd=(w>>0)&31; ",
      "SETX(f_rd,(u64)(pc+(u64)(i64)((f_immhi<<2)|f_immlo)));",
      "" },
};

/* ADRP: specialized by no fields. */
static const SnxA64Case cases_ADRP[] = {
    { SNX_FLOW_PLAIN,
      "const i64 f_immhi=(i64)((i32)(w<<8)>>13); const u32 f_immlo=(w>>29)&3; const u32 f_rd=(w>>0)&31; ",
      "SETX(f_rd,(u64)((pc&~0xFFFull)+((u64)(i64)((f_immhi<<2)|f_immlo)<<12)));",
      "" },
};

/* ADDSUB_IMM: specialized by sf, op, S, sh. */
static const SnxA64Case cases_ADDSUB_IMM[] = {
    /* sf=0, op=0, S=0, sh=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "c->x[f_rd]=(u64)(u32)((u32)c->x[f_rn]+(u32)f_imm12);",
      "" },
    /* sf=0, op=0, S=0, sh=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "c->x[f_rd]=(u64)(u32)((u32)c->x[f_rn]+(u32)(f_imm12<<12));",
      "" },
    /* sf=0, op=0, S=1, sh=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(adds32(c,(u32)c->x[f_rn],(u32)f_imm12,0))));",
      "" },
    /* sf=0, op=0, S=1, sh=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(adds32(c,(u32)c->x[f_rn],(u32)(f_imm12<<12),0))));",
      "" },
    /* sf=0, op=1, S=0, sh=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "c->x[f_rd]=(u64)(u32)((u32)c->x[f_rn]-(u32)f_imm12);",
      "" },
    /* sf=0, op=1, S=0, sh=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "c->x[f_rd]=(u64)(u32)((u32)c->x[f_rn]-(u32)(f_imm12<<12));",
      "" },
    /* sf=0, op=1, S=1, sh=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(subs32(c,(u32)c->x[f_rn],(u32)f_imm12))));",
      "" },
    /* sf=0, op=1, S=1, sh=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(subs32(c,(u32)c->x[f_rn],(u32)(f_imm12<<12)))));",
      "" },
    /* sf=1, op=0, S=0, sh=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "c->x[f_rd]=(u64)(c->x[f_rn]+f_imm12);",
      "" },
    /* sf=1, op=0, S=0, sh=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "c->x[f_rd]=(u64)(c->x[f_rn]+(f_imm12<<12));",
      "" },
    /* sf=1, op=0, S=1, sh=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(adds64(c,c->x[f_rn],f_imm12,0)));",
      "" },
    /* sf=1, op=0, S=1, sh=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(adds64(c,c->x[f_rn],(f_imm12<<12),0)));",
      "" },
    /* sf=1, op=1, S=0, sh=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "c->x[f_rd]=(u64)(c->x[f_rn]-f_imm12);",
      "" },
    /* sf=1, op=1, S=0, sh=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "c->x[f_rd]=(u64)(c->x[f_rn]-(f_imm12<<12));",
      "" },
    /* sf=1, op=1, S=1, sh=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(subs64(c,c->x[f_rn],f_imm12)));",
      "" },
    /* sf=1, op=1, S=1, sh=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(subs64(c,c->x[f_rn],(f_imm12<<12))));",
      "" },
};

/* LOGIC_IMM: specialized by sf, opc. */
static const SnxA64Case cases_LOGIC_IMM[] = {
    /* sf=0, opc=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_N=(w>>22)&1; const u32 f_immr=(w>>16)&63; const u32 f_imms=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "c->x[f_rd]=(u64)(u32)(((u32)RX(f_rn)&snx_decode_bitmask(f_N,f_imms,f_immr,32)));",
      "" },
    /* sf=0, opc=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_N=(w>>22)&1; const u32 f_immr=(w>>16)&63; const u32 f_imms=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "c->x[f_rd]=(u64)(u32)(((u32)RX(f_rn)|snx_decode_bitmask(f_N,f_imms,f_immr,32)));",
      "" },
    /* sf=0, opc=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_N=(w>>22)&1; const u32 f_immr=(w>>16)&63; const u32 f_imms=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "c->x[f_rd]=(u64)(u32)(((u32)RX(f_rn)^snx_decode_bitmask(f_N,f_imms,f_immr,32)));",
      "" },
    /* sf=0, opc=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_N=(w>>22)&1; const u32 f_immr=(w>>16)&63; const u32 f_imms=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "{ u32 r_=(u32)((u32)RX(f_rn)&snx_decode_bitmask(f_N,f_imms,f_immr,32)); logic32(c,r_); SETX(f_rd,(u64)((u32)(r_))); }",
      "" },
    /* sf=1, opc=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_N=(w>>22)&1; const u32 f_immr=(w>>16)&63; const u32 f_imms=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "c->x[f_rd]=(u64)((RX(f_rn)&snx_decode_bitmask(f_N,f_imms,f_immr,64)));",
      "" },
    /* sf=1, opc=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_N=(w>>22)&1; const u32 f_immr=(w>>16)&63; const u32 f_imms=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "c->x[f_rd]=(u64)((RX(f_rn)|snx_decode_bitmask(f_N,f_imms,f_immr,64)));",
      "" },
    /* sf=1, opc=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_N=(w>>22)&1; const u32 f_immr=(w>>16)&63; const u32 f_imms=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "c->x[f_rd]=(u64)((RX(f_rn)^snx_decode_bitmask(f_N,f_imms,f_immr,64)));",
      "" },
    /* sf=1, opc=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_N=(w>>22)&1; const u32 f_immr=(w>>16)&63; const u32 f_imms=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "{ u64 r_=(RX(f_rn)&snx_decode_bitmask(f_N,f_imms,f_immr,64)); logic64(c,r_); SETX(f_rd,(u64)(r_)); }",
      "" },
};

/* MOVW: specialized by sf, opc. */
static const SnxA64Case cases_MOVW[] = {
    /* sf=0, opc=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_hw=(w>>21)&3; const u32 f_imm16=(w>>5)&65535; const u32 f_rd=(w>>0)&31; ",
      "SETX(f_rd,(u64)((u32)(~((u64)f_imm16<<(f_hw*16)))));",
      "" },
    /* sf=0, opc=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, opc=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_hw=(w>>21)&3; const u32 f_imm16=(w>>5)&65535; const u32 f_rd=(w>>0)&31; ",
      "SETX(f_rd,(u64)((u32)(((u64)f_imm16<<(f_hw*16)))));",
      "" },
    /* sf=0, opc=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_hw=(w>>21)&3; const u32 f_imm16=(w>>5)&65535; const u32 f_rd=(w>>0)&31; ",
      "SETX(f_rd,(u64)((u32)((RX(f_rd)&~(0xFFFFull<<(f_hw*16)))|((u64)f_imm16<<(f_hw*16)))));",
      "" },
    /* sf=1, opc=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_hw=(w>>21)&3; const u32 f_imm16=(w>>5)&65535; const u32 f_rd=(w>>0)&31; ",
      "SETX(f_rd,(u64)(~((u64)f_imm16<<(f_hw*16))));",
      "" },
    /* sf=1, opc=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, opc=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_hw=(w>>21)&3; const u32 f_imm16=(w>>5)&65535; const u32 f_rd=(w>>0)&31; ",
      "SETX(f_rd,(u64)(((u64)f_imm16<<(f_hw*16))));",
      "" },
    /* sf=1, opc=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_hw=(w>>21)&3; const u32 f_imm16=(w>>5)&65535; const u32 f_rd=(w>>0)&31; ",
      "SETX(f_rd,(u64)((RX(f_rd)&~(0xFFFFull<<(f_hw*16)))|((u64)f_imm16<<(f_hw*16))));",
      "" },
};

/* BITFIELD: specialized by sf, opc. */
static const SnxA64Case cases_BITFIELD[] = {
    /* sf=0, opc=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_immr=(w>>16)&63; const u32 f_imms=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(sbfm_((u32)RX(f_rn),f_immr,f_imms,32))));",
      "" },
    /* sf=0, opc=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_immr=(w>>16)&63; const u32 f_imms=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(bfm_((u32)RX(f_rd),(u32)RX(f_rn),f_immr,f_imms,32))));",
      "" },
    /* sf=0, opc=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_immr=(w>>16)&63; const u32 f_imms=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(ubfm_((u32)RX(f_rn),f_immr,f_imms,32))));",
      "" },
    /* sf=0, opc=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, opc=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_immr=(w>>16)&63; const u32 f_imms=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(sbfm_(RX(f_rn),f_immr,f_imms,64)));",
      "" },
    /* sf=1, opc=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_immr=(w>>16)&63; const u32 f_imms=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(bfm_(RX(f_rd),RX(f_rn),f_immr,f_imms,64)));",
      "" },
    /* sf=1, opc=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_immr=(w>>16)&63; const u32 f_imms=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(ubfm_(RX(f_rn),f_immr,f_imms,64)));",
      "" },
    /* sf=1, opc=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
};

/* EXTR: specialized by sf. */
static const SnxA64Case cases_EXTR[] = {
    /* sf=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imms=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(f_imms? ((u32)(u32)RX(f_rm)>>f_imms)|((u32)(u32)RX(f_rn)<<(32-f_imms)) : (u32)RX(f_rm))));",
      "" },
    /* sf=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imms=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(f_imms? (RX(f_rm)>>f_imms)|(RX(f_rn)<<(64-f_imms)) : RX(f_rm)));",
      "" },
};

/* B: specialized by no fields. */
static const SnxA64Case cases_B[] = {
    { SNX_FLOW_B,
      "const i64 f_imm26=(i64)((i32)(w<<6)>>6); ",
      "pc+(u64)((i64)f_imm26*4)",
      "" },
};

/* BL: specialized by no fields. */
static const SnxA64Case cases_BL[] = {
    { SNX_FLOW_BL,
      "const i64 f_imm26=(i64)((i32)(w<<6)>>6); ",
      "pc+(u64)((i64)f_imm26*4)",
      "" },
};

/* BCOND: specialized by no fields. */
static const SnxA64Case cases_BCOND[] = {
    { SNX_FLOW_BC,
      "const u32 f_bcond=(w>>0)&15; const i64 f_imm19=(i64)((i32)(w<<8)>>13); ",
      "cond_ok(c,f_bcond)",
      "pc+(u64)((i64)f_imm19*4)" },
};

/* CBZ: specialized by sf, b24. */
static const SnxA64Case cases_CBZ[] = {
    /* sf=0, b24=0 */
    { SNX_FLOW_BC,
      "const i64 f_imm19=(i64)((i32)(w<<8)>>13); const u32 f_rt=(w>>0)&31; ",
      "((u32)RX(f_rt)==0)",
      "pc+(u64)((i64)f_imm19*4)" },
    /* sf=0, b24=1 */
    { SNX_FLOW_BC,
      "const i64 f_imm19=(i64)((i32)(w<<8)>>13); const u32 f_rt=(w>>0)&31; ",
      "((u32)RX(f_rt)!=0)",
      "pc+(u64)((i64)f_imm19*4)" },
    /* sf=1, b24=0 */
    { SNX_FLOW_BC,
      "const i64 f_imm19=(i64)((i32)(w<<8)>>13); const u32 f_rt=(w>>0)&31; ",
      "(RX(f_rt)==0)",
      "pc+(u64)((i64)f_imm19*4)" },
    /* sf=1, b24=1 */
    { SNX_FLOW_BC,
      "const i64 f_imm19=(i64)((i32)(w<<8)>>13); const u32 f_rt=(w>>0)&31; ",
      "(RX(f_rt)!=0)",
      "pc+(u64)((i64)f_imm19*4)" },
};

/* TBZ: specialized by b24. */
static const SnxA64Case cases_TBZ[] = {
    /* b24=0 */
    { SNX_FLOW_BC,
      "const u32 f_b40=(w>>19)&31; const u32 f_b5=(w>>31)&1; const i64 f_imm14=(i64)((i32)(w<<13)>>18); const u32 f_rt=(w>>0)&31; ",
      "!((RX(f_rt)>>((f_b5<<5)|f_b40))&1)",
      "pc+(u64)((i64)f_imm14*4)" },
    /* b24=1 */
    { SNX_FLOW_BC,
      "const u32 f_b40=(w>>19)&31; const u32 f_b5=(w>>31)&1; const i64 f_imm14=(i64)((i32)(w<<13)>>18); const u32 f_rt=(w>>0)&31; ",
      "((RX(f_rt)>>((f_b5<<5)|f_b40))&1)",
      "pc+(u64)((i64)f_imm14*4)" },
};

/* BR: specialized by no fields. */
static const SnxA64Case cases_BR[] = {
    { SNX_FLOW_BR,
      "const u32 f_rn=(w>>5)&31; ",
      "RX(f_rn)",
      "" },
};

/* BLR: specialized by no fields. */
static const SnxA64Case cases_BLR[] = {
    { SNX_FLOW_BLR,
      "const u32 f_rn=(w>>5)&31; ",
      "RX(f_rn)",
      "" },
};

/* RET: specialized by no fields. */
static const SnxA64Case cases_RET[] = {
    { SNX_FLOW_RET,
      "const u32 f_rn=(w>>5)&31; ",
      "RX(f_rn)",
      "" },
};

/* SVC: specialized by no fields. */
static const SnxA64Case cases_SVC[] = {
    { SNX_FLOW_SVC,
      "const u32 f_imm16=(w>>5)&65535; ",
      "f_imm16",
      "" },
};

/* BRK: specialized by no fields. */
static const SnxA64Case cases_BRK[] = {
    { SNX_FLOW_BRK,
      "const u32 f_imm16=(w>>5)&65535; ",
      "f_imm16",
      "" },
};

/* HINT: specialized by no fields. */
static const SnxA64Case cases_HINT[] = {
    { SNX_FLOW_PLAIN,
      "const u32 f_CRm=(w>>8)&15; const u32 f_op2=(w>>5)&7; ",
      "{ u32 k_=(f_CRm<<3)|f_op2; if(k_==1) YIELD(); else if(k_==2) c->rt->wfe(c); else if(k_==4||k_==5) c->rt->sev(); }",
      "" },
};

/* CLREX: specialized by no fields. */
static const SnxA64Case cases_CLREX[] = {
    { SNX_FLOW_PLAIN,
      "",
      "c->excl_on=0;",
      "" },
};

/* BARRIER: specialized by no fields. */
static const SnxA64Case cases_BARRIER[] = {
    { SNX_FLOW_PLAIN,
      "",
      "SNX_FENCE();",
      "" },
};

/* MSR_IMM: specialized by no fields. */
static const SnxA64Case cases_MSR_IMM[] = {
    { SNX_FLOW_PLAIN,
      "",
      "",
      "" },
};

/* SYS: specialized by no fields. */
static const SnxA64Case cases_SYS[] = {
    { SNX_FLOW_PLAIN,
      "const u32 f_CRm=(w>>8)&15; const u32 f_CRn=(w>>12)&15; const u32 f_op1=(w>>16)&7; const u32 f_op2=(w>>5)&7; const u32 f_rt=(w>>0)&31; ",
      "if(f_op1==3&&f_CRn==7&&f_CRm==4&&f_op2==1) dc_zva(M,RX(f_rt));",
      "" },
};

/* MRS: specialized by no fields. */
static const SnxA64Case cases_MRS[] = {
    { SNX_FLOW_PLAIN,
      "const u32 f_rt=(w>>0)&31; const u32 f_sysreg=(w>>5)&32767; ",
      "SETX(f_rt,(u64)(snx_mrs(c,f_sysreg)));",
      "" },
};

/* MSR: specialized by no fields. */
static const SnxA64Case cases_MSR[] = {
    { SNX_FLOW_PLAIN,
      "const u32 f_rt=(w>>0)&31; const u32 f_sysreg=(w>>5)&32767; ",
      "snx_msr(c,f_sysreg,RX(f_rt));",
      "" },
};

/* LDST_UIMM: specialized by size, V, lopc. */
static const SnxA64Case cases_LDST_UIMM[] = {
    /* size=0, V=0, lopc=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR8((c->x[f_rn]+((u64)f_imm12<<0)),RX(f_rt));",
      "" },
    /* size=0, V=0, lopc=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(RD8((c->x[f_rn]+((u64)f_imm12<<0)))));",
      "" },
    /* size=0, V=0, lopc=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)((u64)(i64)RDS8((c->x[f_rn]+((u64)f_imm12<<0)))));",
      "" },
    /* size=0, V=0, lopc=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)((u32)((u32)(i32)RDS8((c->x[f_rn]+((u64)f_imm12<<0))))));",
      "" },
    /* size=0, V=1, lopc=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR8((c->x[f_rn]+((u64)f_imm12<<0)),c->v[f_rt].d[0]);",
      "" },
    /* size=0, V=1, lopc=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 t_=RD8((c->x[f_rn]+((u64)f_imm12<<0))); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; }",
      "" },
    /* size=0, V=1, lopc=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WRQ((c->x[f_rn]+((u64)f_imm12<<4)),c->v[f_rt]);",
      "" },
    /* size=0, V=1, lopc=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "RDQ(c->v[f_rt],(c->x[f_rn]+((u64)f_imm12<<4)));",
      "" },
    /* size=1, V=0, lopc=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR16((c->x[f_rn]+((u64)f_imm12<<1)),RX(f_rt));",
      "" },
    /* size=1, V=0, lopc=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(RD16((c->x[f_rn]+((u64)f_imm12<<1)))));",
      "" },
    /* size=1, V=0, lopc=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)((u64)(i64)RDS16((c->x[f_rn]+((u64)f_imm12<<1)))));",
      "" },
    /* size=1, V=0, lopc=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)((u32)((u32)(i32)RDS16((c->x[f_rn]+((u64)f_imm12<<1))))));",
      "" },
    /* size=1, V=1, lopc=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR16((c->x[f_rn]+((u64)f_imm12<<1)),c->v[f_rt].d[0]);",
      "" },
    /* size=1, V=1, lopc=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 t_=RD16((c->x[f_rn]+((u64)f_imm12<<1))); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; }",
      "" },
    /* size=1, V=1, lopc=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=1, V=1, lopc=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=2, V=0, lopc=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR32((c->x[f_rn]+((u64)f_imm12<<2)),RX(f_rt));",
      "" },
    /* size=2, V=0, lopc=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(RD32((c->x[f_rn]+((u64)f_imm12<<2)))));",
      "" },
    /* size=2, V=0, lopc=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)((u64)(i64)RDS32((c->x[f_rn]+((u64)f_imm12<<2)))));",
      "" },
    /* size=2, V=0, lopc=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=2, V=1, lopc=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR32((c->x[f_rn]+((u64)f_imm12<<2)),c->v[f_rt].d[0]);",
      "" },
    /* size=2, V=1, lopc=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 t_=RD32((c->x[f_rn]+((u64)f_imm12<<2))); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; }",
      "" },
    /* size=2, V=1, lopc=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=2, V=1, lopc=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=3, V=0, lopc=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR64((c->x[f_rn]+((u64)f_imm12<<3)),RX(f_rt));",
      "" },
    /* size=3, V=0, lopc=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(RD64((c->x[f_rn]+((u64)f_imm12<<3)))));",
      "" },
    /* size=3, V=0, lopc=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=3, V=0, lopc=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=3, V=1, lopc=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR64((c->x[f_rn]+((u64)f_imm12<<3)),c->v[f_rt].d[0]);",
      "" },
    /* size=3, V=1, lopc=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 t_=RD64((c->x[f_rn]+((u64)f_imm12<<3))); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; }",
      "" },
    /* size=3, V=1, lopc=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=3, V=1, lopc=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_imm12=(w>>10)&4095; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
};

/* LDST_IMM9: specialized by size, V, lopc, imode. */
static const SnxA64Case cases_LDST_IMM9[] = {
    /* size=0, V=0, lopc=0, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR8((c->x[f_rn]+(u64)(i64)f_imm9),RX(f_rt));",
      "" },
    /* size=0, V=0, lopc=0, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; WR8(a_,RX(f_rt)); c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=0, V=0, lopc=0, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR8((c->x[f_rn]+(u64)(i64)f_imm9),RX(f_rt));",
      "" },
    /* size=0, V=0, lopc=0, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; WR8(a_,RX(f_rt)); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=0, V=0, lopc=1, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(RD8((c->x[f_rn]+(u64)(i64)f_imm9))));",
      "" },
    /* size=0, V=0, lopc=1, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; SETX(f_rt,(u64)(RD8(a_))); c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=0, V=0, lopc=1, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(RD8((c->x[f_rn]+(u64)(i64)f_imm9))));",
      "" },
    /* size=0, V=0, lopc=1, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; SETX(f_rt,(u64)(RD8(a_))); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=0, V=0, lopc=2, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)((u64)(i64)RDS8((c->x[f_rn]+(u64)(i64)f_imm9))));",
      "" },
    /* size=0, V=0, lopc=2, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; SETX(f_rt,(u64)((u64)(i64)RDS8(a_))); c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=0, V=0, lopc=2, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)((u64)(i64)RDS8((c->x[f_rn]+(u64)(i64)f_imm9))));",
      "" },
    /* size=0, V=0, lopc=2, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; SETX(f_rt,(u64)((u64)(i64)RDS8(a_))); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=0, V=0, lopc=3, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)((u32)((u32)(i32)RDS8((c->x[f_rn]+(u64)(i64)f_imm9)))));",
      "" },
    /* size=0, V=0, lopc=3, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; SETX(f_rt,(u64)((u32)((u32)(i32)RDS8(a_)))); c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=0, V=0, lopc=3, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)((u32)((u32)(i32)RDS8((c->x[f_rn]+(u64)(i64)f_imm9)))));",
      "" },
    /* size=0, V=0, lopc=3, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; SETX(f_rt,(u64)((u32)((u32)(i32)RDS8(a_)))); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=0, V=1, lopc=0, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR8((c->x[f_rn]+(u64)(i64)f_imm9),c->v[f_rt].d[0]);",
      "" },
    /* size=0, V=1, lopc=0, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; WR8(a_,c->v[f_rt].d[0]); c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=0, V=1, lopc=0, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR8((c->x[f_rn]+(u64)(i64)f_imm9),c->v[f_rt].d[0]);",
      "" },
    /* size=0, V=1, lopc=0, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; WR8(a_,c->v[f_rt].d[0]); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=0, V=1, lopc=1, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 t_=RD8((c->x[f_rn]+(u64)(i64)f_imm9)); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; }",
      "" },
    /* size=0, V=1, lopc=1, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; { u64 t_=RD8(a_); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; } c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=0, V=1, lopc=1, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 t_=RD8((c->x[f_rn]+(u64)(i64)f_imm9)); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; }",
      "" },
    /* size=0, V=1, lopc=1, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; { u64 t_=RD8(a_); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; } c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=0, V=1, lopc=2, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WRQ((c->x[f_rn]+(u64)(i64)f_imm9),c->v[f_rt]);",
      "" },
    /* size=0, V=1, lopc=2, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; WRQ(a_,c->v[f_rt]); c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=0, V=1, lopc=2, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WRQ((c->x[f_rn]+(u64)(i64)f_imm9),c->v[f_rt]);",
      "" },
    /* size=0, V=1, lopc=2, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; WRQ(a_,c->v[f_rt]); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=0, V=1, lopc=3, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "RDQ(c->v[f_rt],(c->x[f_rn]+(u64)(i64)f_imm9));",
      "" },
    /* size=0, V=1, lopc=3, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; RDQ(c->v[f_rt],a_); c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=0, V=1, lopc=3, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "RDQ(c->v[f_rt],(c->x[f_rn]+(u64)(i64)f_imm9));",
      "" },
    /* size=0, V=1, lopc=3, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; RDQ(c->v[f_rt],a_); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=1, V=0, lopc=0, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR16((c->x[f_rn]+(u64)(i64)f_imm9),RX(f_rt));",
      "" },
    /* size=1, V=0, lopc=0, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; WR16(a_,RX(f_rt)); c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=1, V=0, lopc=0, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR16((c->x[f_rn]+(u64)(i64)f_imm9),RX(f_rt));",
      "" },
    /* size=1, V=0, lopc=0, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; WR16(a_,RX(f_rt)); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=1, V=0, lopc=1, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(RD16((c->x[f_rn]+(u64)(i64)f_imm9))));",
      "" },
    /* size=1, V=0, lopc=1, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; SETX(f_rt,(u64)(RD16(a_))); c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=1, V=0, lopc=1, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(RD16((c->x[f_rn]+(u64)(i64)f_imm9))));",
      "" },
    /* size=1, V=0, lopc=1, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; SETX(f_rt,(u64)(RD16(a_))); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=1, V=0, lopc=2, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)((u64)(i64)RDS16((c->x[f_rn]+(u64)(i64)f_imm9))));",
      "" },
    /* size=1, V=0, lopc=2, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; SETX(f_rt,(u64)((u64)(i64)RDS16(a_))); c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=1, V=0, lopc=2, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)((u64)(i64)RDS16((c->x[f_rn]+(u64)(i64)f_imm9))));",
      "" },
    /* size=1, V=0, lopc=2, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; SETX(f_rt,(u64)((u64)(i64)RDS16(a_))); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=1, V=0, lopc=3, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)((u32)((u32)(i32)RDS16((c->x[f_rn]+(u64)(i64)f_imm9)))));",
      "" },
    /* size=1, V=0, lopc=3, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; SETX(f_rt,(u64)((u32)((u32)(i32)RDS16(a_)))); c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=1, V=0, lopc=3, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)((u32)((u32)(i32)RDS16((c->x[f_rn]+(u64)(i64)f_imm9)))));",
      "" },
    /* size=1, V=0, lopc=3, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; SETX(f_rt,(u64)((u32)((u32)(i32)RDS16(a_)))); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=1, V=1, lopc=0, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR16((c->x[f_rn]+(u64)(i64)f_imm9),c->v[f_rt].d[0]);",
      "" },
    /* size=1, V=1, lopc=0, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; WR16(a_,c->v[f_rt].d[0]); c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=1, V=1, lopc=0, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR16((c->x[f_rn]+(u64)(i64)f_imm9),c->v[f_rt].d[0]);",
      "" },
    /* size=1, V=1, lopc=0, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; WR16(a_,c->v[f_rt].d[0]); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=1, V=1, lopc=1, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 t_=RD16((c->x[f_rn]+(u64)(i64)f_imm9)); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; }",
      "" },
    /* size=1, V=1, lopc=1, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; { u64 t_=RD16(a_); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; } c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=1, V=1, lopc=1, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 t_=RD16((c->x[f_rn]+(u64)(i64)f_imm9)); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; }",
      "" },
    /* size=1, V=1, lopc=1, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; { u64 t_=RD16(a_); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; } c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=1, V=1, lopc=2, imode=0 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=1, V=1, lopc=2, imode=1 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=1, V=1, lopc=2, imode=2 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=1, V=1, lopc=2, imode=3 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=1, V=1, lopc=3, imode=0 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=1, V=1, lopc=3, imode=1 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=1, V=1, lopc=3, imode=2 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=1, V=1, lopc=3, imode=3 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=2, V=0, lopc=0, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR32((c->x[f_rn]+(u64)(i64)f_imm9),RX(f_rt));",
      "" },
    /* size=2, V=0, lopc=0, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; WR32(a_,RX(f_rt)); c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=2, V=0, lopc=0, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR32((c->x[f_rn]+(u64)(i64)f_imm9),RX(f_rt));",
      "" },
    /* size=2, V=0, lopc=0, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; WR32(a_,RX(f_rt)); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=2, V=0, lopc=1, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(RD32((c->x[f_rn]+(u64)(i64)f_imm9))));",
      "" },
    /* size=2, V=0, lopc=1, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; SETX(f_rt,(u64)(RD32(a_))); c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=2, V=0, lopc=1, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(RD32((c->x[f_rn]+(u64)(i64)f_imm9))));",
      "" },
    /* size=2, V=0, lopc=1, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; SETX(f_rt,(u64)(RD32(a_))); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=2, V=0, lopc=2, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)((u64)(i64)RDS32((c->x[f_rn]+(u64)(i64)f_imm9))));",
      "" },
    /* size=2, V=0, lopc=2, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; SETX(f_rt,(u64)((u64)(i64)RDS32(a_))); c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=2, V=0, lopc=2, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)((u64)(i64)RDS32((c->x[f_rn]+(u64)(i64)f_imm9))));",
      "" },
    /* size=2, V=0, lopc=2, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; SETX(f_rt,(u64)((u64)(i64)RDS32(a_))); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=2, V=0, lopc=3, imode=0 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=2, V=0, lopc=3, imode=1 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); ",
      "",
      "" },
    /* size=2, V=0, lopc=3, imode=2 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=2, V=0, lopc=3, imode=3 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); ",
      "",
      "" },
    /* size=2, V=1, lopc=0, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR32((c->x[f_rn]+(u64)(i64)f_imm9),c->v[f_rt].d[0]);",
      "" },
    /* size=2, V=1, lopc=0, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; WR32(a_,c->v[f_rt].d[0]); c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=2, V=1, lopc=0, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR32((c->x[f_rn]+(u64)(i64)f_imm9),c->v[f_rt].d[0]);",
      "" },
    /* size=2, V=1, lopc=0, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; WR32(a_,c->v[f_rt].d[0]); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=2, V=1, lopc=1, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 t_=RD32((c->x[f_rn]+(u64)(i64)f_imm9)); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; }",
      "" },
    /* size=2, V=1, lopc=1, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; { u64 t_=RD32(a_); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; } c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=2, V=1, lopc=1, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 t_=RD32((c->x[f_rn]+(u64)(i64)f_imm9)); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; }",
      "" },
    /* size=2, V=1, lopc=1, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; { u64 t_=RD32(a_); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; } c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=2, V=1, lopc=2, imode=0 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=2, V=1, lopc=2, imode=1 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=2, V=1, lopc=2, imode=2 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=2, V=1, lopc=2, imode=3 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=2, V=1, lopc=3, imode=0 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=2, V=1, lopc=3, imode=1 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=2, V=1, lopc=3, imode=2 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=2, V=1, lopc=3, imode=3 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=3, V=0, lopc=0, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR64((c->x[f_rn]+(u64)(i64)f_imm9),RX(f_rt));",
      "" },
    /* size=3, V=0, lopc=0, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; WR64(a_,RX(f_rt)); c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=3, V=0, lopc=0, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR64((c->x[f_rn]+(u64)(i64)f_imm9),RX(f_rt));",
      "" },
    /* size=3, V=0, lopc=0, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; WR64(a_,RX(f_rt)); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=3, V=0, lopc=1, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(RD64((c->x[f_rn]+(u64)(i64)f_imm9))));",
      "" },
    /* size=3, V=0, lopc=1, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; SETX(f_rt,(u64)(RD64(a_))); c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=3, V=0, lopc=1, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(RD64((c->x[f_rn]+(u64)(i64)f_imm9))));",
      "" },
    /* size=3, V=0, lopc=1, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; SETX(f_rt,(u64)(RD64(a_))); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=3, V=0, lopc=2, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=3, V=0, lopc=2, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; ",
      "{ u64 a_=c->x[f_rn];  c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=3, V=0, lopc=2, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=3, V=0, lopc=2, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9;  c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=3, V=0, lopc=3, imode=0 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=3, V=0, lopc=3, imode=1 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); ",
      "",
      "" },
    /* size=3, V=0, lopc=3, imode=2 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=3, V=0, lopc=3, imode=3 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); ",
      "",
      "" },
    /* size=3, V=1, lopc=0, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR64((c->x[f_rn]+(u64)(i64)f_imm9),c->v[f_rt].d[0]);",
      "" },
    /* size=3, V=1, lopc=0, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; WR64(a_,c->v[f_rt].d[0]); c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=3, V=1, lopc=0, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR64((c->x[f_rn]+(u64)(i64)f_imm9),c->v[f_rt].d[0]);",
      "" },
    /* size=3, V=1, lopc=0, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; WR64(a_,c->v[f_rt].d[0]); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=3, V=1, lopc=1, imode=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 t_=RD64((c->x[f_rn]+(u64)(i64)f_imm9)); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; }",
      "" },
    /* size=3, V=1, lopc=1, imode=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]; { u64 t_=RD64(a_); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; } c->x[f_rn]=(u64)(a_+(u64)(i64)f_imm9); }",
      "" },
    /* size=3, V=1, lopc=1, imode=2 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 t_=RD64((c->x[f_rn]+(u64)(i64)f_imm9)); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; }",
      "" },
    /* size=3, V=1, lopc=1, imode=3 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 a_=c->x[f_rn]+(u64)(i64)f_imm9; { u64 t_=RD64(a_); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; } c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=3, V=1, lopc=2, imode=0 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=3, V=1, lopc=2, imode=1 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=3, V=1, lopc=2, imode=2 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=3, V=1, lopc=2, imode=3 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=3, V=1, lopc=3, imode=0 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=3, V=1, lopc=3, imode=1 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=3, V=1, lopc=3, imode=2 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=3, V=1, lopc=3, imode=3 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm9=(i64)((i32)(w<<11)>>23); const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
};

/* LDST_REG: specialized by size, V, lopc. */
static const SnxA64Case cases_LDST_REG[] = {
    /* size=0, V=0, lopc=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR8((c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?0:0)),RX(f_rt));",
      "" },
    /* size=0, V=0, lopc=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(RD8((c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?0:0)))));",
      "" },
    /* size=0, V=0, lopc=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)((u64)(i64)RDS8((c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?0:0)))));",
      "" },
    /* size=0, V=0, lopc=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)((u32)((u32)(i32)RDS8((c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?0:0))))));",
      "" },
    /* size=0, V=1, lopc=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR8((c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?0:0)),c->v[f_rt].d[0]);",
      "" },
    /* size=0, V=1, lopc=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 t_=RD8((c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?0:0))); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; }",
      "" },
    /* size=0, V=1, lopc=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WRQ((c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?4:0)),c->v[f_rt]);",
      "" },
    /* size=0, V=1, lopc=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "RDQ(c->v[f_rt],(c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?4:0)));",
      "" },
    /* size=1, V=0, lopc=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR16((c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?1:0)),RX(f_rt));",
      "" },
    /* size=1, V=0, lopc=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(RD16((c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?1:0)))));",
      "" },
    /* size=1, V=0, lopc=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)((u64)(i64)RDS16((c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?1:0)))));",
      "" },
    /* size=1, V=0, lopc=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)((u32)((u32)(i32)RDS16((c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?1:0))))));",
      "" },
    /* size=1, V=1, lopc=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR16((c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?1:0)),c->v[f_rt].d[0]);",
      "" },
    /* size=1, V=1, lopc=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 t_=RD16((c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?1:0))); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; }",
      "" },
    /* size=1, V=1, lopc=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=1, V=1, lopc=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=2, V=0, lopc=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR32((c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?2:0)),RX(f_rt));",
      "" },
    /* size=2, V=0, lopc=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(RD32((c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?2:0)))));",
      "" },
    /* size=2, V=0, lopc=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)((u64)(i64)RDS32((c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?2:0)))));",
      "" },
    /* size=2, V=0, lopc=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=2, V=1, lopc=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR32((c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?2:0)),c->v[f_rt].d[0]);",
      "" },
    /* size=2, V=1, lopc=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 t_=RD32((c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?2:0))); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; }",
      "" },
    /* size=2, V=1, lopc=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=2, V=1, lopc=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=3, V=0, lopc=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR64((c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?3:0)),RX(f_rt));",
      "" },
    /* size=3, V=0, lopc=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(RD64((c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?3:0)))));",
      "" },
    /* size=3, V=0, lopc=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=3, V=0, lopc=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=3, V=1, lopc=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "WR64((c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?3:0)),c->v[f_rt].d[0]);",
      "" },
    /* size=3, V=1, lopc=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "{ u64 t_=RD64((c->x[f_rn]+ext64(RX(f_rm),f_option,f_Sb?3:0))); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; }",
      "" },
    /* size=3, V=1, lopc=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
    /* size=3, V=1, lopc=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_Sb=(w>>12)&1; const u32 f_option=(w>>13)&7; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "",
      "" },
};

/* LDR_LIT: specialized by lopc_lit, V. */
static const SnxA64Case cases_LDR_LIT[] = {
    /* lopc_lit=0, V=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm19=(i64)((i32)(w<<8)>>13); const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(RD32((pc+(u64)((i64)f_imm19*4)))));",
      "" },
    /* lopc_lit=0, V=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm19=(i64)((i32)(w<<8)>>13); const u32 f_rt=(w>>0)&31; ",
      "{ u64 t_=RD32((pc+(u64)((i64)f_imm19*4))); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; }",
      "" },
    /* lopc_lit=1, V=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm19=(i64)((i32)(w<<8)>>13); const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(RD64((pc+(u64)((i64)f_imm19*4)))));",
      "" },
    /* lopc_lit=1, V=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm19=(i64)((i32)(w<<8)>>13); const u32 f_rt=(w>>0)&31; ",
      "{ u64 t_=RD64((pc+(u64)((i64)f_imm19*4))); c->v[f_rt].d[1]=0; c->v[f_rt].d[0]=t_; }",
      "" },
    /* lopc_lit=2, V=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm19=(i64)((i32)(w<<8)>>13); const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)((u64)(i64)RDS32((pc+(u64)((i64)f_imm19*4)))));",
      "" },
    /* lopc_lit=2, V=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm19=(i64)((i32)(w<<8)>>13); const u32 f_rt=(w>>0)&31; ",
      "RDQ(c->v[f_rt],(pc+(u64)((i64)f_imm19*4)));",
      "" },
    /* lopc_lit=3, V=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm19=(i64)((i32)(w<<8)>>13); ",
      "",
      "" },
    /* lopc_lit=3, V=1 */
    { SNX_FLOW_UNDEF,
      "const i64 f_imm19=(i64)((i32)(w<<8)>>13); ",
      "",
      "" },
};

/* LDSTP: specialized by size, V, mode, L. */
static const SnxA64Case cases_LDSTP[] = {
    /* size=0, V=0, mode=0, L=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*4); WR32(a_,RX(f_rt)); WR32(a_+4,RX(f_rt2));  }",
      "" },
    /* size=0, V=0, mode=0, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*4); u64 v1_=RD32(a_), v2_=RD32(a_+4); SETX(f_rt,(u64)(v1_)); SETX(f_rt2,(u64)(v2_));  }",
      "" },
    /* size=0, V=0, mode=1, L=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]; WR32(a_,RX(f_rt)); WR32(a_+4,RX(f_rt2)); c->x[f_rn]=(u64)(a_+((u64)(i64)f_imm7*4)); }",
      "" },
    /* size=0, V=0, mode=1, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]; u64 v1_=RD32(a_), v2_=RD32(a_+4); SETX(f_rt,(u64)(v1_)); SETX(f_rt2,(u64)(v2_)); c->x[f_rn]=(u64)(a_+((u64)(i64)f_imm7*4)); }",
      "" },
    /* size=0, V=0, mode=2, L=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*4); WR32(a_,RX(f_rt)); WR32(a_+4,RX(f_rt2));  }",
      "" },
    /* size=0, V=0, mode=2, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*4); u64 v1_=RD32(a_), v2_=RD32(a_+4); SETX(f_rt,(u64)(v1_)); SETX(f_rt2,(u64)(v2_));  }",
      "" },
    /* size=0, V=0, mode=3, L=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*4); WR32(a_,RX(f_rt)); WR32(a_+4,RX(f_rt2)); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=0, V=0, mode=3, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*4); u64 v1_=RD32(a_), v2_=RD32(a_+4); SETX(f_rt,(u64)(v1_)); SETX(f_rt2,(u64)(v2_)); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=0, V=1, mode=0, L=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*4); WR32(a_,c->v[f_rt].d[0]); WR32(a_+4,c->v[f_rt2].d[0]);  }",
      "" },
    /* size=0, V=1, mode=0, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*4); u64 v1_=RD32(a_), v2_=RD32(a_+4); c->v[f_rt].d[0]=v1_; c->v[f_rt].d[1]=0; c->v[f_rt2].d[0]=v2_; c->v[f_rt2].d[1]=0;  }",
      "" },
    /* size=0, V=1, mode=1, L=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]; WR32(a_,c->v[f_rt].d[0]); WR32(a_+4,c->v[f_rt2].d[0]); c->x[f_rn]=(u64)(a_+((u64)(i64)f_imm7*4)); }",
      "" },
    /* size=0, V=1, mode=1, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]; u64 v1_=RD32(a_), v2_=RD32(a_+4); c->v[f_rt].d[0]=v1_; c->v[f_rt].d[1]=0; c->v[f_rt2].d[0]=v2_; c->v[f_rt2].d[1]=0; c->x[f_rn]=(u64)(a_+((u64)(i64)f_imm7*4)); }",
      "" },
    /* size=0, V=1, mode=2, L=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*4); WR32(a_,c->v[f_rt].d[0]); WR32(a_+4,c->v[f_rt2].d[0]);  }",
      "" },
    /* size=0, V=1, mode=2, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*4); u64 v1_=RD32(a_), v2_=RD32(a_+4); c->v[f_rt].d[0]=v1_; c->v[f_rt].d[1]=0; c->v[f_rt2].d[0]=v2_; c->v[f_rt2].d[1]=0;  }",
      "" },
    /* size=0, V=1, mode=3, L=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*4); WR32(a_,c->v[f_rt].d[0]); WR32(a_+4,c->v[f_rt2].d[0]); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=0, V=1, mode=3, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*4); u64 v1_=RD32(a_), v2_=RD32(a_+4); c->v[f_rt].d[0]=v1_; c->v[f_rt].d[1]=0; c->v[f_rt2].d[0]=v2_; c->v[f_rt2].d[1]=0; c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=1, V=0, mode=0, L=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* size=1, V=0, mode=0, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*4); u64 v1_=(u64)(i64)RDS32(a_), v2_=(u64)(i64)RDS32(a_+4); SETX(f_rt,(u64)(v1_)); SETX(f_rt2,(u64)(v2_));  }",
      "" },
    /* size=1, V=0, mode=1, L=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* size=1, V=0, mode=1, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]; u64 v1_=(u64)(i64)RDS32(a_), v2_=(u64)(i64)RDS32(a_+4); SETX(f_rt,(u64)(v1_)); SETX(f_rt2,(u64)(v2_)); c->x[f_rn]=(u64)(a_+((u64)(i64)f_imm7*4)); }",
      "" },
    /* size=1, V=0, mode=2, L=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* size=1, V=0, mode=2, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*4); u64 v1_=(u64)(i64)RDS32(a_), v2_=(u64)(i64)RDS32(a_+4); SETX(f_rt,(u64)(v1_)); SETX(f_rt2,(u64)(v2_));  }",
      "" },
    /* size=1, V=0, mode=3, L=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* size=1, V=0, mode=3, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*4); u64 v1_=(u64)(i64)RDS32(a_), v2_=(u64)(i64)RDS32(a_+4); SETX(f_rt,(u64)(v1_)); SETX(f_rt2,(u64)(v2_)); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=1, V=1, mode=0, L=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*8); WR64(a_,c->v[f_rt].d[0]); WR64(a_+8,c->v[f_rt2].d[0]);  }",
      "" },
    /* size=1, V=1, mode=0, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*8); u64 v1_=RD64(a_), v2_=RD64(a_+8); c->v[f_rt].d[0]=v1_; c->v[f_rt].d[1]=0; c->v[f_rt2].d[0]=v2_; c->v[f_rt2].d[1]=0;  }",
      "" },
    /* size=1, V=1, mode=1, L=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]; WR64(a_,c->v[f_rt].d[0]); WR64(a_+8,c->v[f_rt2].d[0]); c->x[f_rn]=(u64)(a_+((u64)(i64)f_imm7*8)); }",
      "" },
    /* size=1, V=1, mode=1, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]; u64 v1_=RD64(a_), v2_=RD64(a_+8); c->v[f_rt].d[0]=v1_; c->v[f_rt].d[1]=0; c->v[f_rt2].d[0]=v2_; c->v[f_rt2].d[1]=0; c->x[f_rn]=(u64)(a_+((u64)(i64)f_imm7*8)); }",
      "" },
    /* size=1, V=1, mode=2, L=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*8); WR64(a_,c->v[f_rt].d[0]); WR64(a_+8,c->v[f_rt2].d[0]);  }",
      "" },
    /* size=1, V=1, mode=2, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*8); u64 v1_=RD64(a_), v2_=RD64(a_+8); c->v[f_rt].d[0]=v1_; c->v[f_rt].d[1]=0; c->v[f_rt2].d[0]=v2_; c->v[f_rt2].d[1]=0;  }",
      "" },
    /* size=1, V=1, mode=3, L=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*8); WR64(a_,c->v[f_rt].d[0]); WR64(a_+8,c->v[f_rt2].d[0]); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=1, V=1, mode=3, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*8); u64 v1_=RD64(a_), v2_=RD64(a_+8); c->v[f_rt].d[0]=v1_; c->v[f_rt].d[1]=0; c->v[f_rt2].d[0]=v2_; c->v[f_rt2].d[1]=0; c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=2, V=0, mode=0, L=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*8); WR64(a_,RX(f_rt)); WR64(a_+8,RX(f_rt2));  }",
      "" },
    /* size=2, V=0, mode=0, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*8); u64 v1_=RD64(a_), v2_=RD64(a_+8); SETX(f_rt,(u64)(v1_)); SETX(f_rt2,(u64)(v2_));  }",
      "" },
    /* size=2, V=0, mode=1, L=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]; WR64(a_,RX(f_rt)); WR64(a_+8,RX(f_rt2)); c->x[f_rn]=(u64)(a_+((u64)(i64)f_imm7*8)); }",
      "" },
    /* size=2, V=0, mode=1, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]; u64 v1_=RD64(a_), v2_=RD64(a_+8); SETX(f_rt,(u64)(v1_)); SETX(f_rt2,(u64)(v2_)); c->x[f_rn]=(u64)(a_+((u64)(i64)f_imm7*8)); }",
      "" },
    /* size=2, V=0, mode=2, L=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*8); WR64(a_,RX(f_rt)); WR64(a_+8,RX(f_rt2));  }",
      "" },
    /* size=2, V=0, mode=2, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*8); u64 v1_=RD64(a_), v2_=RD64(a_+8); SETX(f_rt,(u64)(v1_)); SETX(f_rt2,(u64)(v2_));  }",
      "" },
    /* size=2, V=0, mode=3, L=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*8); WR64(a_,RX(f_rt)); WR64(a_+8,RX(f_rt2)); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=2, V=0, mode=3, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*8); u64 v1_=RD64(a_), v2_=RD64(a_+8); SETX(f_rt,(u64)(v1_)); SETX(f_rt2,(u64)(v2_)); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=2, V=1, mode=0, L=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*16); WRQ(a_,c->v[f_rt]); WRQ(a_+16,c->v[f_rt2]);  }",
      "" },
    /* size=2, V=1, mode=0, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*16); V128 q1_,q2_; RDQ(q1_,a_); RDQ(q2_,a_+16); c->v[f_rt]=q1_; c->v[f_rt2]=q2_;  }",
      "" },
    /* size=2, V=1, mode=1, L=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]; WRQ(a_,c->v[f_rt]); WRQ(a_+16,c->v[f_rt2]); c->x[f_rn]=(u64)(a_+((u64)(i64)f_imm7*16)); }",
      "" },
    /* size=2, V=1, mode=1, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]; V128 q1_,q2_; RDQ(q1_,a_); RDQ(q2_,a_+16); c->v[f_rt]=q1_; c->v[f_rt2]=q2_; c->x[f_rn]=(u64)(a_+((u64)(i64)f_imm7*16)); }",
      "" },
    /* size=2, V=1, mode=2, L=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*16); WRQ(a_,c->v[f_rt]); WRQ(a_+16,c->v[f_rt2]);  }",
      "" },
    /* size=2, V=1, mode=2, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*16); V128 q1_,q2_; RDQ(q1_,a_); RDQ(q2_,a_+16); c->v[f_rt]=q1_; c->v[f_rt2]=q2_;  }",
      "" },
    /* size=2, V=1, mode=3, L=0 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*16); WRQ(a_,c->v[f_rt]); WRQ(a_+16,c->v[f_rt2]); c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=2, V=1, mode=3, L=1 */
    { SNX_FLOW_PLAIN,
      "const i64 f_imm7=(i64)((i32)(w<<10)>>25); const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 a_=c->x[f_rn]+((u64)(i64)f_imm7*16); V128 q1_,q2_; RDQ(q1_,a_); RDQ(q2_,a_+16); c->v[f_rt]=q1_; c->v[f_rt2]=q2_; c->x[f_rn]=(u64)(a_); }",
      "" },
    /* size=3, V=0, mode=0, L=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* size=3, V=0, mode=0, L=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* size=3, V=0, mode=1, L=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* size=3, V=0, mode=1, L=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* size=3, V=0, mode=2, L=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* size=3, V=0, mode=2, L=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* size=3, V=0, mode=3, L=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* size=3, V=0, mode=3, L=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* size=3, V=1, mode=0, L=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* size=3, V=1, mode=0, L=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* size=3, V=1, mode=1, L=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* size=3, V=1, mode=1, L=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* size=3, V=1, mode=2, L=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* size=3, V=1, mode=2, L=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* size=3, V=1, mode=3, L=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* size=3, V=1, mode=3, L=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
};

/* LDSTX: specialized by size, o2, L, o1. */
static const SnxA64Case cases_LDSTX[] = {
    /* size=0, o2=0, L=0, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; const u32 f_rs=(w>>16)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rs,(u64)((u32)(stx(c,M,c->x[f_rn],0,RX(f_rt)))));",
      "" },
    /* size=0, o2=0, L=0, o1=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=0, o2=0, L=1, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(ldx(c,M,c->x[f_rn],0)));",
      "" },
    /* size=0, o2=0, L=1, o1=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=0, o2=1, L=0, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "STREL(u8,c->x[f_rn],RX(f_rt));",
      "" },
    /* size=0, o2=1, L=0, o1=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=0, o2=1, L=1, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(LDACQ(u8,c->x[f_rn])));",
      "" },
    /* size=0, o2=1, L=1, o1=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=1, o2=0, L=0, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; const u32 f_rs=(w>>16)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rs,(u64)((u32)(stx(c,M,c->x[f_rn],1,RX(f_rt)))));",
      "" },
    /* size=1, o2=0, L=0, o1=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=1, o2=0, L=1, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(ldx(c,M,c->x[f_rn],1)));",
      "" },
    /* size=1, o2=0, L=1, o1=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=1, o2=1, L=0, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "STREL(u16,c->x[f_rn],RX(f_rt));",
      "" },
    /* size=1, o2=1, L=0, o1=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=1, o2=1, L=1, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(LDACQ(u16,c->x[f_rn])));",
      "" },
    /* size=1, o2=1, L=1, o1=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=2, o2=0, L=0, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; const u32 f_rs=(w>>16)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rs,(u64)((u32)(stx(c,M,c->x[f_rn],2,RX(f_rt)))));",
      "" },
    /* size=2, o2=0, L=0, o1=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; const u32 f_rs=(w>>16)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "SETX(f_rs,(u64)((u32)(stxp(c,M,c->x[f_rn],2,RX(f_rt),RX(f_rt2)))));",
      "" },
    /* size=2, o2=0, L=1, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(ldx(c,M,c->x[f_rn],2)));",
      "" },
    /* size=2, o2=0, L=1, o1=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 v1_,v2_; ldxp(c,M,c->x[f_rn],2,&v1_,&v2_); SETX(f_rt,(u64)(v1_)); SETX(f_rt2,(u64)(v2_)); }",
      "" },
    /* size=2, o2=1, L=0, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "STREL(u32,c->x[f_rn],RX(f_rt));",
      "" },
    /* size=2, o2=1, L=0, o1=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=2, o2=1, L=1, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(LDACQ(u32,c->x[f_rn])));",
      "" },
    /* size=2, o2=1, L=1, o1=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=3, o2=0, L=0, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; const u32 f_rs=(w>>16)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rs,(u64)((u32)(stx(c,M,c->x[f_rn],3,RX(f_rt)))));",
      "" },
    /* size=3, o2=0, L=0, o1=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; const u32 f_rs=(w>>16)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "SETX(f_rs,(u64)((u32)(stxp(c,M,c->x[f_rn],3,RX(f_rt),RX(f_rt2)))));",
      "" },
    /* size=3, o2=0, L=1, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(ldx(c,M,c->x[f_rn],3)));",
      "" },
    /* size=3, o2=0, L=1, o1=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; const u32 f_rt2=(w>>10)&31; ",
      "{ u64 v1_,v2_; ldxp(c,M,c->x[f_rn],3,&v1_,&v2_); SETX(f_rt,(u64)(v1_)); SETX(f_rt2,(u64)(v2_)); }",
      "" },
    /* size=3, o2=1, L=0, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "STREL(u64,c->x[f_rn],RX(f_rt));",
      "" },
    /* size=3, o2=1, L=0, o1=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* size=3, o2=1, L=1, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; const u32 f_rt=(w>>0)&31; ",
      "SETX(f_rt,(u64)(LDACQ(u64,c->x[f_rn])));",
      "" },
    /* size=3, o2=1, L=1, o1=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
};

/* LOGIC_REG: specialized by sf, opc, o1. */
static const SnxA64Case cases_LOGIC_REG[] = {
    /* sf=0, opc=0, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "SETX(f_rd,(u64)((u32)(((u32)RX(f_rn)&shf32((u32)RX(f_rm),f_shift,f_imm6)))));",
      "" },
    /* sf=0, opc=0, o1=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "SETX(f_rd,(u64)((u32)(((u32)RX(f_rn)&(~shf32((u32)RX(f_rm),f_shift,f_imm6))))));",
      "" },
    /* sf=0, opc=1, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "SETX(f_rd,(u64)((u32)(((u32)RX(f_rn)|shf32((u32)RX(f_rm),f_shift,f_imm6)))));",
      "" },
    /* sf=0, opc=1, o1=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "SETX(f_rd,(u64)((u32)(((u32)RX(f_rn)|(~shf32((u32)RX(f_rm),f_shift,f_imm6))))));",
      "" },
    /* sf=0, opc=2, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "SETX(f_rd,(u64)((u32)(((u32)RX(f_rn)^shf32((u32)RX(f_rm),f_shift,f_imm6)))));",
      "" },
    /* sf=0, opc=2, o1=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "SETX(f_rd,(u64)((u32)(((u32)RX(f_rn)^(~shf32((u32)RX(f_rm),f_shift,f_imm6))))));",
      "" },
    /* sf=0, opc=3, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "{ u32 r_=(u32)((u32)RX(f_rn)&shf32((u32)RX(f_rm),f_shift,f_imm6)); logic32(c,r_); SETX(f_rd,(u64)((u32)(r_))); }",
      "" },
    /* sf=0, opc=3, o1=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "{ u32 r_=(u32)((u32)RX(f_rn)&(~shf32((u32)RX(f_rm),f_shift,f_imm6))); logic32(c,r_); SETX(f_rd,(u64)((u32)(r_))); }",
      "" },
    /* sf=1, opc=0, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "SETX(f_rd,(u64)((RX(f_rn)&shf64(RX(f_rm),f_shift,f_imm6))));",
      "" },
    /* sf=1, opc=0, o1=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "SETX(f_rd,(u64)((RX(f_rn)&(~shf64(RX(f_rm),f_shift,f_imm6)))));",
      "" },
    /* sf=1, opc=1, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "SETX(f_rd,(u64)((RX(f_rn)|shf64(RX(f_rm),f_shift,f_imm6))));",
      "" },
    /* sf=1, opc=1, o1=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "SETX(f_rd,(u64)((RX(f_rn)|(~shf64(RX(f_rm),f_shift,f_imm6)))));",
      "" },
    /* sf=1, opc=2, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "SETX(f_rd,(u64)((RX(f_rn)^shf64(RX(f_rm),f_shift,f_imm6))));",
      "" },
    /* sf=1, opc=2, o1=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "SETX(f_rd,(u64)((RX(f_rn)^(~shf64(RX(f_rm),f_shift,f_imm6)))));",
      "" },
    /* sf=1, opc=3, o1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "{ u64 r_=(RX(f_rn)&shf64(RX(f_rm),f_shift,f_imm6)); logic64(c,r_); SETX(f_rd,(u64)(r_)); }",
      "" },
    /* sf=1, opc=3, o1=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "{ u64 r_=(RX(f_rn)&(~shf64(RX(f_rm),f_shift,f_imm6))); logic64(c,r_); SETX(f_rd,(u64)(r_)); }",
      "" },
};

/* ADDSUB_SREG: specialized by sf, op, S. */
static const SnxA64Case cases_ADDSUB_SREG[] = {
    /* sf=0, op=0, S=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "SETX(f_rd,(u64)((u32)((u32)(u32)RX(f_rn)+(u32)shf32((u32)RX(f_rm),f_shift,f_imm6))));",
      "" },
    /* sf=0, op=0, S=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "SETX(f_rd,(u64)((u32)(adds32(c,(u32)RX(f_rn),shf32((u32)RX(f_rm),f_shift,f_imm6),0))));",
      "" },
    /* sf=0, op=1, S=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "SETX(f_rd,(u64)((u32)((u32)(u32)RX(f_rn)-(u32)shf32((u32)RX(f_rm),f_shift,f_imm6))));",
      "" },
    /* sf=0, op=1, S=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "SETX(f_rd,(u64)((u32)(subs32(c,(u32)RX(f_rn),shf32((u32)RX(f_rm),f_shift,f_imm6)))));",
      "" },
    /* sf=1, op=0, S=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "SETX(f_rd,(u64)(RX(f_rn)+shf64(RX(f_rm),f_shift,f_imm6)));",
      "" },
    /* sf=1, op=0, S=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "SETX(f_rd,(u64)(adds64(c,RX(f_rn),shf64(RX(f_rm),f_shift,f_imm6),0)));",
      "" },
    /* sf=1, op=1, S=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "SETX(f_rd,(u64)(RX(f_rn)-shf64(RX(f_rm),f_shift,f_imm6)));",
      "" },
    /* sf=1, op=1, S=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm6=(w>>10)&63; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; const u32 f_shift=(w>>22)&3; ",
      "SETX(f_rd,(u64)(subs64(c,RX(f_rn),shf64(RX(f_rm),f_shift,f_imm6))));",
      "" },
};

/* ADDSUB_EXT: specialized by sf, op, S. */
static const SnxA64Case cases_ADDSUB_EXT[] = {
    /* sf=0, op=0, S=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm3=(w>>10)&7; const u32 f_option=(w>>13)&7; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "c->x[f_rd]=(u64)(u32)((u32)c->x[f_rn]+(u32)ext64(RX(f_rm),f_option,f_imm3));",
      "" },
    /* sf=0, op=0, S=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm3=(w>>10)&7; const u32 f_option=(w>>13)&7; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(adds32(c,(u32)c->x[f_rn],(u32)ext64(RX(f_rm),f_option,f_imm3),0))));",
      "" },
    /* sf=0, op=1, S=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm3=(w>>10)&7; const u32 f_option=(w>>13)&7; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "c->x[f_rd]=(u64)(u32)((u32)c->x[f_rn]-(u32)ext64(RX(f_rm),f_option,f_imm3));",
      "" },
    /* sf=0, op=1, S=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm3=(w>>10)&7; const u32 f_option=(w>>13)&7; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(subs32(c,(u32)c->x[f_rn],(u32)ext64(RX(f_rm),f_option,f_imm3)))));",
      "" },
    /* sf=1, op=0, S=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm3=(w>>10)&7; const u32 f_option=(w>>13)&7; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "c->x[f_rd]=(u64)(c->x[f_rn]+ext64(RX(f_rm),f_option,f_imm3));",
      "" },
    /* sf=1, op=0, S=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm3=(w>>10)&7; const u32 f_option=(w>>13)&7; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(adds64(c,c->x[f_rn],ext64(RX(f_rm),f_option,f_imm3),0)));",
      "" },
    /* sf=1, op=1, S=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm3=(w>>10)&7; const u32 f_option=(w>>13)&7; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "c->x[f_rd]=(u64)(c->x[f_rn]-ext64(RX(f_rm),f_option,f_imm3));",
      "" },
    /* sf=1, op=1, S=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm3=(w>>10)&7; const u32 f_option=(w>>13)&7; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(subs64(c,c->x[f_rn],ext64(RX(f_rm),f_option,f_imm3))));",
      "" },
};

/* ADC: specialized by sf, op, S. */
static const SnxA64Case cases_ADC[] = {
    /* sf=0, op=0, S=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)((u32)(u32)RX(f_rn)+(u32)(u32)RX(f_rm)+c->cf)));",
      "" },
    /* sf=0, op=0, S=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(adds32(c,(u32)RX(f_rn),(u32)RX(f_rm),c->cf))));",
      "" },
    /* sf=0, op=1, S=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)((u32)(u32)RX(f_rn)+~(u32)(u32)RX(f_rm)+c->cf)));",
      "" },
    /* sf=0, op=1, S=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(sbcs32(c,(u32)RX(f_rn),(u32)RX(f_rm),c->cf))));",
      "" },
    /* sf=1, op=0, S=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u64)RX(f_rn)+(u64)RX(f_rm)+c->cf));",
      "" },
    /* sf=1, op=0, S=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(adds64(c,RX(f_rn),RX(f_rm),c->cf)));",
      "" },
    /* sf=1, op=1, S=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u64)RX(f_rn)+~(u64)RX(f_rm)+c->cf));",
      "" },
    /* sf=1, op=1, S=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(sbcs64(c,RX(f_rn),RX(f_rm),c->cf)));",
      "" },
};

/* CCMP: specialized by sf, op, b11. */
static const SnxA64Case cases_CCMP[] = {
    /* sf=0, op=0, b11=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_cond=(w>>12)&15; const u32 f_nzcv=(w>>0)&15; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "if(cond_ok(c,f_cond)) (void)adds32(c,(u32)RX(f_rn),(u32)RX(f_rm),0); else set_nzcv(c,f_nzcv);",
      "" },
    /* sf=0, op=0, b11=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_cond=(w>>12)&15; const u32 f_imm5=(w>>16)&31; const u32 f_nzcv=(w>>0)&15; const u32 f_rn=(w>>5)&31; ",
      "if(cond_ok(c,f_cond)) (void)adds32(c,(u32)RX(f_rn),f_imm5,0); else set_nzcv(c,f_nzcv);",
      "" },
    /* sf=0, op=1, b11=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_cond=(w>>12)&15; const u32 f_nzcv=(w>>0)&15; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "if(cond_ok(c,f_cond)) (void)subs32(c,(u32)RX(f_rn),(u32)RX(f_rm)); else set_nzcv(c,f_nzcv);",
      "" },
    /* sf=0, op=1, b11=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_cond=(w>>12)&15; const u32 f_imm5=(w>>16)&31; const u32 f_nzcv=(w>>0)&15; const u32 f_rn=(w>>5)&31; ",
      "if(cond_ok(c,f_cond)) (void)subs32(c,(u32)RX(f_rn),f_imm5); else set_nzcv(c,f_nzcv);",
      "" },
    /* sf=1, op=0, b11=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_cond=(w>>12)&15; const u32 f_nzcv=(w>>0)&15; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "if(cond_ok(c,f_cond)) (void)adds64(c,RX(f_rn),RX(f_rm),0); else set_nzcv(c,f_nzcv);",
      "" },
    /* sf=1, op=0, b11=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_cond=(w>>12)&15; const u32 f_imm5=(w>>16)&31; const u32 f_nzcv=(w>>0)&15; const u32 f_rn=(w>>5)&31; ",
      "if(cond_ok(c,f_cond)) (void)adds64(c,RX(f_rn),f_imm5,0); else set_nzcv(c,f_nzcv);",
      "" },
    /* sf=1, op=1, b11=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_cond=(w>>12)&15; const u32 f_nzcv=(w>>0)&15; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "if(cond_ok(c,f_cond)) (void)subs64(c,RX(f_rn),RX(f_rm)); else set_nzcv(c,f_nzcv);",
      "" },
    /* sf=1, op=1, b11=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_cond=(w>>12)&15; const u32 f_imm5=(w>>16)&31; const u32 f_nzcv=(w>>0)&15; const u32 f_rn=(w>>5)&31; ",
      "if(cond_ok(c,f_cond)) (void)subs64(c,RX(f_rn),f_imm5); else set_nzcv(c,f_nzcv);",
      "" },
};

/* CSEL: specialized by sf, op, b10. */
static const SnxA64Case cases_CSEL[] = {
    /* sf=0, op=0, b10=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_cond=(w>>12)&15; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(cond_ok(c,f_cond)? (u32)RX(f_rn) : (u32)RX(f_rm))));",
      "" },
    /* sf=0, op=0, b10=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_cond=(w>>12)&15; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(cond_ok(c,f_cond)? (u32)RX(f_rn) : (u32)((u32)RX(f_rm)+1))));",
      "" },
    /* sf=0, op=1, b10=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_cond=(w>>12)&15; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(cond_ok(c,f_cond)? (u32)RX(f_rn) : (u32)~(u32)RX(f_rm))));",
      "" },
    /* sf=0, op=1, b10=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_cond=(w>>12)&15; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(cond_ok(c,f_cond)? (u32)RX(f_rn) : (u32)(0-(u32)RX(f_rm)))));",
      "" },
    /* sf=1, op=0, b10=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_cond=(w>>12)&15; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(cond_ok(c,f_cond)? RX(f_rn) : RX(f_rm)));",
      "" },
    /* sf=1, op=0, b10=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_cond=(w>>12)&15; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(cond_ok(c,f_cond)? RX(f_rn) : (u64)(RX(f_rm)+1)));",
      "" },
    /* sf=1, op=1, b10=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_cond=(w>>12)&15; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(cond_ok(c,f_cond)? RX(f_rn) : (u64)~RX(f_rm)));",
      "" },
    /* sf=1, op=1, b10=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_cond=(w>>12)&15; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(cond_ok(c,f_cond)? RX(f_rn) : (u64)(0-RX(f_rm))));",
      "" },
};

/* DP1: specialized by sf, dpop. */
static const SnxA64Case cases_DP1[] = {
    /* sf=0, dpop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(rbit32((u32)RX(f_rn)))));",
      "" },
    /* sf=0, dpop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)((u32)rev16_64((u32)RX(f_rn)))));",
      "" },
    /* sf=0, dpop=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(bswap32((u32)RX(f_rn)))));",
      "" },
    /* sf=0, dpop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=4 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(clz32((u32)RX(f_rn)))));",
      "" },
    /* sf=0, dpop=5 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(cls32((u32)RX(f_rn)))));",
      "" },
    /* sf=0, dpop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=8 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=9 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=10 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=11 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=12 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=13 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=14 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=15 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=16 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=17 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=18 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=19 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=20 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=21 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=22 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=23 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=24 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=25 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=26 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=27 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=28 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=29 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=30 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=31 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=32 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=33 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=34 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=35 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=36 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=37 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=38 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=39 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=40 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=41 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=42 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=43 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=44 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=45 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=46 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=47 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=48 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=49 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=50 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=51 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=52 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=53 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=54 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=55 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=56 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=57 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=58 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=59 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=60 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=61 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=62 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=63 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(rbit64(RX(f_rn))));",
      "" },
    /* sf=1, dpop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(rev16_64(RX(f_rn))));",
      "" },
    /* sf=1, dpop=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(rev32_64(RX(f_rn))));",
      "" },
    /* sf=1, dpop=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(bswap64(RX(f_rn))));",
      "" },
    /* sf=1, dpop=4 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(clz64(RX(f_rn))));",
      "" },
    /* sf=1, dpop=5 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(cls64(RX(f_rn))));",
      "" },
    /* sf=1, dpop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=8 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=9 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=10 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=11 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=12 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=13 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=14 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=15 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=16 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=17 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=18 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=19 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=20 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=21 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=22 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=23 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=24 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=25 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=26 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=27 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=28 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=29 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=30 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=31 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=32 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=33 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=34 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=35 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=36 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=37 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=38 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=39 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=40 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=41 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=42 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=43 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=44 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=45 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=46 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=47 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=48 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=49 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=50 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=51 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=52 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=53 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=54 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=55 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=56 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=57 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=58 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=59 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=60 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=61 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=62 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=63 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
};

/* DP2: specialized by sf, dpop. */
static const SnxA64Case cases_DP2[] = {
    /* sf=0, dpop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(udiv32((u32)RX(f_rn),(u32)RX(f_rm)))));",
      "" },
    /* sf=0, dpop=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)((u32)sdiv32((i32)(u32)RX(f_rn),(i32)(u32)RX(f_rm)))));",
      "" },
    /* sf=0, dpop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=8 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)((u32)(u32)RX(f_rn)<<((u32)RX(f_rm)&31))));",
      "" },
    /* sf=0, dpop=9 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)((u32)(u32)RX(f_rn)>>((u32)RX(f_rm)&31))));",
      "" },
    /* sf=0, dpop=10 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)((u32)((i32)(u32)RX(f_rn)>>((u32)RX(f_rm)&31)))));",
      "" },
    /* sf=0, dpop=11 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(ror32((u32)RX(f_rn),(u32)RX(f_rm)))));",
      "" },
    /* sf=0, dpop=12 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=13 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=14 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=15 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=16 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(snx_crc32((u32)RX(f_rn),(u32)RX(f_rm),1,0))));",
      "" },
    /* sf=0, dpop=17 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(snx_crc32((u32)RX(f_rn),(u32)RX(f_rm),2,0))));",
      "" },
    /* sf=0, dpop=18 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(snx_crc32((u32)RX(f_rn),(u32)RX(f_rm),4,0))));",
      "" },
    /* sf=0, dpop=19 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=20 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(snx_crc32((u32)RX(f_rn),(u32)RX(f_rm),1,1))));",
      "" },
    /* sf=0, dpop=21 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(snx_crc32((u32)RX(f_rn),(u32)RX(f_rm),2,1))));",
      "" },
    /* sf=0, dpop=22 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(snx_crc32((u32)RX(f_rn),(u32)RX(f_rm),4,1))));",
      "" },
    /* sf=0, dpop=23 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=24 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=25 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=26 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=27 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=28 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=29 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=30 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=31 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=32 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=33 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=34 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=35 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=36 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=37 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=38 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=39 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=40 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=41 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=42 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=43 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=44 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=45 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=46 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=47 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=48 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=49 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=50 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=51 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=52 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=53 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=54 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=55 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=56 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=57 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=58 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=59 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=60 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=61 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=62 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, dpop=63 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(udiv64(RX(f_rn),RX(f_rm))));",
      "" },
    /* sf=1, dpop=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u64)sdiv64((i64)RX(f_rn),(i64)RX(f_rm))));",
      "" },
    /* sf=1, dpop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=8 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u64)RX(f_rn)<<(RX(f_rm)&63)));",
      "" },
    /* sf=1, dpop=9 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u64)RX(f_rn)>>(RX(f_rm)&63)));",
      "" },
    /* sf=1, dpop=10 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u64)((i64)RX(f_rn)>>(RX(f_rm)&63))));",
      "" },
    /* sf=1, dpop=11 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(ror64(RX(f_rn),(u32)RX(f_rm))));",
      "" },
    /* sf=1, dpop=12 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=13 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=14 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=15 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=16 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=17 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=18 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=19 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(snx_crc32((u32)RX(f_rn),RX(f_rm),8,0))));",
      "" },
    /* sf=1, dpop=20 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=21 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=22 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=23 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)(snx_crc32((u32)RX(f_rn),RX(f_rm),8,1))));",
      "" },
    /* sf=1, dpop=24 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=25 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=26 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=27 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=28 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=29 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=30 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=31 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=32 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=33 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=34 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=35 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=36 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=37 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=38 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=39 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=40 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=41 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=42 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=43 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=44 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=45 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=46 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=47 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=48 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=49 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=50 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=51 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=52 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=53 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=54 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=55 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=56 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=57 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=58 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=59 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=60 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=61 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=62 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, dpop=63 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
};

/* DP3: specialized by sf, op31, o0. */
static const SnxA64Case cases_DP3[] = {
    /* sf=0, op31=0, o0=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_ra=(w>>10)&31; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)((u32)(u32)RX(f_ra)+(u32)(u32)RX(f_rn)*(u32)(u32)RX(f_rm))));",
      "" },
    /* sf=0, op31=0, o0=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_ra=(w>>10)&31; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u32)((u32)(u32)RX(f_ra)-(u32)(u32)RX(f_rn)*(u32)(u32)RX(f_rm))));",
      "" },
    /* sf=0, op31=1, o0=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, op31=1, o0=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, op31=2, o0=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, op31=2, o0=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, op31=3, o0=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, op31=3, o0=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, op31=4, o0=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, op31=4, o0=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, op31=5, o0=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, op31=5, o0=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, op31=6, o0=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, op31=6, o0=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, op31=7, o0=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, op31=7, o0=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, op31=0, o0=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_ra=(w>>10)&31; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(RX(f_ra)+RX(f_rn)*RX(f_rm)));",
      "" },
    /* sf=1, op31=0, o0=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_ra=(w>>10)&31; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(RX(f_ra)-RX(f_rn)*RX(f_rm)));",
      "" },
    /* sf=1, op31=1, o0=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_ra=(w>>10)&31; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(RX(f_ra)+(u64)((i64)(i32)(u32)RX(f_rn)*(i64)(i32)(u32)RX(f_rm))));",
      "" },
    /* sf=1, op31=1, o0=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_ra=(w>>10)&31; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(RX(f_ra)-(u64)((i64)(i32)(u32)RX(f_rn)*(i64)(i32)(u32)RX(f_rm))));",
      "" },
    /* sf=1, op31=2, o0=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)((u64)smulh64((i64)RX(f_rn),(i64)RX(f_rm))));",
      "" },
    /* sf=1, op31=2, o0=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, op31=3, o0=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, op31=3, o0=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, op31=4, o0=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, op31=4, o0=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, op31=5, o0=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_ra=(w>>10)&31; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(RX(f_ra)+(u64)(u32)(u32)RX(f_rn)*(u64)(u32)(u32)RX(f_rm)));",
      "" },
    /* sf=1, op31=5, o0=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_ra=(w>>10)&31; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(RX(f_ra)-(u64)(u32)(u32)RX(f_rn)*(u64)(u32)(u32)RX(f_rm)));",
      "" },
    /* sf=1, op31=6, o0=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(umulh64(RX(f_rn),RX(f_rm))));",
      "" },
    /* sf=1, op31=6, o0=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, op31=7, o0=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, op31=7, o0=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
};

/* FP2: specialized by ftype, fop2. */
static const SnxA64Case cases_FP2[] = {
    /* ftype=0, fop2=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_binary(c,c->v[f_rn].s[0],c->v[f_rm].s[0],32,SNX_FP_MUL));",
      "" },
    /* ftype=0, fop2=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_binary(c,c->v[f_rn].s[0],c->v[f_rm].s[0],32,SNX_FP_DIV));",
      "" },
    /* ftype=0, fop2=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_binary(c,c->v[f_rn].s[0],c->v[f_rm].s[0],32,SNX_FP_ADD));",
      "" },
    /* ftype=0, fop2=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_binary(c,c->v[f_rn].s[0],c->v[f_rm].s[0],32,SNX_FP_SUB));",
      "" },
    /* ftype=0, fop2=4 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_minmax(c,c->v[f_rn].s[0],c->v[f_rm].s[0],32,0,0));",
      "" },
    /* ftype=0, fop2=5 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_minmax(c,c->v[f_rn].s[0],c->v[f_rm].s[0],32,1,0));",
      "" },
    /* ftype=0, fop2=6 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_minmax(c,c->v[f_rn].s[0],c->v[f_rm].s[0],32,0,1));",
      "" },
    /* ftype=0, fop2=7 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_minmax(c,c->v[f_rn].s[0],c->v[f_rm].s[0],32,1,1));",
      "" },
    /* ftype=0, fop2=8 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_binary(c,c->v[f_rn].s[0],c->v[f_rm].s[0],32,SNX_FP_MUL)^0x80000000u);",
      "" },
    /* ftype=0, fop2=9 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=0, fop2=10 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=0, fop2=11 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=0, fop2=12 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=0, fop2=13 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=0, fop2=14 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=0, fop2=15 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=1, fop2=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_binary(c,c->v[f_rn].d[0],c->v[f_rm].d[0],64,SNX_FP_MUL));",
      "" },
    /* ftype=1, fop2=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_binary(c,c->v[f_rn].d[0],c->v[f_rm].d[0],64,SNX_FP_DIV));",
      "" },
    /* ftype=1, fop2=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_binary(c,c->v[f_rn].d[0],c->v[f_rm].d[0],64,SNX_FP_ADD));",
      "" },
    /* ftype=1, fop2=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_binary(c,c->v[f_rn].d[0],c->v[f_rm].d[0],64,SNX_FP_SUB));",
      "" },
    /* ftype=1, fop2=4 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_minmax(c,c->v[f_rn].d[0],c->v[f_rm].d[0],64,0,0));",
      "" },
    /* ftype=1, fop2=5 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_minmax(c,c->v[f_rn].d[0],c->v[f_rm].d[0],64,1,0));",
      "" },
    /* ftype=1, fop2=6 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_minmax(c,c->v[f_rn].d[0],c->v[f_rm].d[0],64,0,1));",
      "" },
    /* ftype=1, fop2=7 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_minmax(c,c->v[f_rn].d[0],c->v[f_rm].d[0],64,1,1));",
      "" },
    /* ftype=1, fop2=8 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_binary(c,c->v[f_rn].d[0],c->v[f_rm].d[0],64,SNX_FP_MUL)^0x8000000000000000ull);",
      "" },
    /* ftype=1, fop2=9 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=1, fop2=10 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=1, fop2=11 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=1, fop2=12 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=1, fop2=13 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=1, fop2=14 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=1, fop2=15 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=2, fop2=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=2, fop2=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=2, fop2=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=2, fop2=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=2, fop2=4 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=2, fop2=5 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=2, fop2=6 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=2, fop2=7 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=2, fop2=8 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=2, fop2=9 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=2, fop2=10 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=2, fop2=11 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=2, fop2=12 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=2, fop2=13 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=2, fop2=14 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=2, fop2=15 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, fop2=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, fop2=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, fop2=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, fop2=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, fop2=4 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, fop2=5 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, fop2=6 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, fop2=7 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, fop2=8 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, fop2=9 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, fop2=10 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, fop2=11 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, fop2=12 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, fop2=13 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, fop2=14 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, fop2=15 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
};

/* FP1: specialized by ftype, fop1. */
static const SnxA64Case cases_FP1[] = {
    /* ftype=0, fop1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,c->v[f_rn].s[0]);",
      "" },
    /* ftype=0, fop1=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,c->v[f_rn].s[0]&0x7FFFFFFFu);",
      "" },
    /* ftype=0, fop1=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,c->v[f_rn].s[0]^0x80000000u);",
      "" },
    /* ftype=0, fop1=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_unary(c,c->v[f_rn].s[0],32,SNX_FP_SQRT));",
      "" },
    /* ftype=0, fop1=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=5 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_convert(c,c->v[f_rn].s[0],32,64,FPRM));",
      "" },
    /* ftype=0, fop1=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=7 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETH(f_rd,snx_fp_convert(c,c->v[f_rn].s[0],32,16,FPRM));",
      "" },
    /* ftype=0, fop1=8 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_round(c,c->v[f_rn].s[0],32,0,0));",
      "" },
    /* ftype=0, fop1=9 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_round(c,c->v[f_rn].s[0],32,1,0));",
      "" },
    /* ftype=0, fop1=10 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_round(c,c->v[f_rn].s[0],32,2,0));",
      "" },
    /* ftype=0, fop1=11 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_round(c,c->v[f_rn].s[0],32,3,0));",
      "" },
    /* ftype=0, fop1=12 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_round(c,c->v[f_rn].s[0],32,4,0));",
      "" },
    /* ftype=0, fop1=13 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=14 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_round(c,c->v[f_rn].s[0],32,FPRM,1));",
      "" },
    /* ftype=0, fop1=15 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_round(c,c->v[f_rn].s[0],32,FPRM,0));",
      "" },
    /* ftype=0, fop1=16 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=17 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=18 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=19 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=20 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=21 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=22 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=23 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=24 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=25 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=26 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=27 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=28 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=29 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=30 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=31 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=32 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=33 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=34 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=35 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=36 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=37 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=38 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=39 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=40 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=41 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=42 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=43 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=44 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=45 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=46 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=47 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=48 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=49 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=50 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=51 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=52 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=53 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=54 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=55 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=56 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=57 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=58 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=59 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=60 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=61 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=62 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=0, fop1=63 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,c->v[f_rn].d[0]);",
      "" },
    /* ftype=1, fop1=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,c->v[f_rn].d[0]&0x7FFFFFFFFFFFFFFFull);",
      "" },
    /* ftype=1, fop1=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,c->v[f_rn].d[0]^0x8000000000000000ull);",
      "" },
    /* ftype=1, fop1=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_unary(c,c->v[f_rn].d[0],64,SNX_FP_SQRT));",
      "" },
    /* ftype=1, fop1=4 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_convert(c,c->v[f_rn].d[0],64,32,FPRM));",
      "" },
    /* ftype=1, fop1=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=7 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETH(f_rd,snx_fp_convert(c,c->v[f_rn].d[0],64,16,FPRM));",
      "" },
    /* ftype=1, fop1=8 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_round(c,c->v[f_rn].d[0],64,0,0));",
      "" },
    /* ftype=1, fop1=9 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_round(c,c->v[f_rn].d[0],64,1,0));",
      "" },
    /* ftype=1, fop1=10 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_round(c,c->v[f_rn].d[0],64,2,0));",
      "" },
    /* ftype=1, fop1=11 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_round(c,c->v[f_rn].d[0],64,3,0));",
      "" },
    /* ftype=1, fop1=12 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_round(c,c->v[f_rn].d[0],64,4,0));",
      "" },
    /* ftype=1, fop1=13 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=14 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_round(c,c->v[f_rn].d[0],64,FPRM,1));",
      "" },
    /* ftype=1, fop1=15 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_round(c,c->v[f_rn].d[0],64,FPRM,0));",
      "" },
    /* ftype=1, fop1=16 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=17 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=18 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=19 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=20 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=21 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=22 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=23 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=24 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=25 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=26 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=27 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=28 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=29 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=30 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=31 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=32 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=33 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=34 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=35 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=36 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=37 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=38 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=39 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=40 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=41 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=42 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=43 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=44 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=45 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=46 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=47 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=48 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=49 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=50 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=51 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=52 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=53 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=54 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=55 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=56 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=57 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=58 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=59 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=60 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=61 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=62 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=1, fop1=63 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=8 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=9 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=10 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=11 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=12 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=13 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=14 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=15 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=16 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=17 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=18 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=19 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=20 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=21 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=22 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=23 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=24 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=25 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=26 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=27 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=28 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=29 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=30 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=31 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=32 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=33 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=34 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=35 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=36 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=37 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=38 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=39 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=40 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=41 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=42 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=43 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=44 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=45 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=46 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=47 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=48 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=49 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=50 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=51 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=52 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=53 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=54 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=55 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=56 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=57 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=58 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=59 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=60 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=61 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=62 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=2, fop1=63 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=4 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_convert(c,c->v[f_rn].h[0],16,32,FPRM));",
      "" },
    /* ftype=3, fop1=5 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_convert(c,c->v[f_rn].h[0],16,64,FPRM));",
      "" },
    /* ftype=3, fop1=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=8 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=9 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=10 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=11 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=12 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=13 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=14 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=15 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=16 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=17 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=18 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=19 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=20 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=21 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=22 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=23 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=24 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=25 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=26 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=27 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=28 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=29 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=30 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=31 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=32 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=33 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=34 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=35 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=36 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=37 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=38 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=39 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=40 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=41 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=42 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=43 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=44 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=45 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=46 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=47 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=48 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=49 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=50 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=51 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=52 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=53 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=54 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=55 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=56 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=57 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=58 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=59 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=60 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=61 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=62 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* ftype=3, fop1=63 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
};

/* FCMP: specialized by ftype, fcmpop. */
static const SnxA64Case cases_FCMP[] = {
    /* ftype=0, fcmpop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "set_nzcv(c,snx_fp_compare(c,c->v[f_rn].s[0],c->v[f_rm].s[0],32,0));",
      "" },
    /* ftype=0, fcmpop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; ",
      "set_nzcv(c,snx_fp_compare(c,c->v[f_rn].s[0],0,32,0));",
      "" },
    /* ftype=0, fcmpop=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "set_nzcv(c,snx_fp_compare(c,c->v[f_rn].s[0],c->v[f_rm].s[0],32,1));",
      "" },
    /* ftype=0, fcmpop=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; ",
      "set_nzcv(c,snx_fp_compare(c,c->v[f_rn].s[0],0,32,1));",
      "" },
    /* ftype=1, fcmpop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "set_nzcv(c,snx_fp_compare(c,c->v[f_rn].d[0],c->v[f_rm].d[0],64,0));",
      "" },
    /* ftype=1, fcmpop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; ",
      "set_nzcv(c,snx_fp_compare(c,c->v[f_rn].d[0],0,64,0));",
      "" },
    /* ftype=1, fcmpop=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "set_nzcv(c,snx_fp_compare(c,c->v[f_rn].d[0],c->v[f_rm].d[0],64,1));",
      "" },
    /* ftype=1, fcmpop=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rn=(w>>5)&31; ",
      "set_nzcv(c,snx_fp_compare(c,c->v[f_rn].d[0],0,64,1));",
      "" },
    /* ftype=2, fcmpop=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=2, fcmpop=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=2, fcmpop=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=2, fcmpop=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, fcmpop=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, fcmpop=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, fcmpop=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, fcmpop=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
};

/* FCCMP: specialized by ftype. */
static const SnxA64Case cases_FCCMP[] = {
    /* ftype=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_cond=(w>>12)&15; const u32 f_nzcv=(w>>0)&15; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "if(cond_ok(c,f_cond)) set_nzcv(c,snx_fp_compare(c,c->v[f_rn].s[0],c->v[f_rm].s[0],32,(w>>4)&1)); else set_nzcv(c,f_nzcv);",
      "" },
    /* ftype=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_cond=(w>>12)&15; const u32 f_nzcv=(w>>0)&15; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "if(cond_ok(c,f_cond)) set_nzcv(c,snx_fp_compare(c,c->v[f_rn].d[0],c->v[f_rm].d[0],64,(w>>4)&1)); else set_nzcv(c,f_nzcv);",
      "" },
    /* ftype=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
};

/* FCSEL: specialized by ftype. */
static const SnxA64Case cases_FCSEL[] = {
    /* ftype=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_cond=(w>>12)&15; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,cond_ok(c,f_cond)? c->v[f_rn].s[0] : c->v[f_rm].s[0]);",
      "" },
    /* ftype=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_cond=(w>>12)&15; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,cond_ok(c,f_cond)? c->v[f_rn].d[0] : c->v[f_rm].d[0]);",
      "" },
    /* ftype=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
};

/* FMOV_IMM: specialized by ftype. */
static const SnxA64Case cases_FMOV_IMM[] = {
    /* ftype=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm8=(w>>13)&255; const u32 f_rd=(w>>0)&31; ",
      "SETSB(f_rd,vfp_imm32(f_imm8));",
      "" },
    /* ftype=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_imm8=(w>>13)&255; const u32 f_rd=(w>>0)&31; ",
      "SETDB(f_rd,vfp_imm64(f_imm8));",
      "" },
    /* ftype=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
};

/* FP3: specialized by ftype, o1, o0. */
static const SnxA64Case cases_FP3[] = {
    /* ftype=0, o1=0, o0=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_ra=(w>>10)&31; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_fma(c,c->v[f_ra].s[0],c->v[f_rn].s[0],c->v[f_rm].s[0],32));",
      "" },
    /* ftype=0, o1=0, o0=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_ra=(w>>10)&31; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_fma(c,c->v[f_ra].s[0],c->v[f_rn].s[0]^0x80000000u,c->v[f_rm].s[0],32));",
      "" },
    /* ftype=0, o1=1, o0=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_ra=(w>>10)&31; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_fma(c,c->v[f_ra].s[0]^0x80000000u,c->v[f_rn].s[0]^0x80000000u,c->v[f_rm].s[0],32));",
      "" },
    /* ftype=0, o1=1, o0=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_ra=(w>>10)&31; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_fma(c,c->v[f_ra].s[0]^0x80000000u,c->v[f_rn].s[0],c->v[f_rm].s[0],32));",
      "" },
    /* ftype=1, o1=0, o0=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_ra=(w>>10)&31; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_fma(c,c->v[f_ra].d[0],c->v[f_rn].d[0],c->v[f_rm].d[0],64));",
      "" },
    /* ftype=1, o1=0, o0=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_ra=(w>>10)&31; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_fma(c,c->v[f_ra].d[0],c->v[f_rn].d[0]^0x8000000000000000ull,c->v[f_rm].d[0],64));",
      "" },
    /* ftype=1, o1=1, o0=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_ra=(w>>10)&31; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_fma(c,c->v[f_ra].d[0]^0x8000000000000000ull,c->v[f_rn].d[0]^0x8000000000000000ull,c->v[f_rm].d[0],64));",
      "" },
    /* ftype=1, o1=1, o0=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_ra=(w>>10)&31; const u32 f_rd=(w>>0)&31; const u32 f_rm=(w>>16)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_fma(c,c->v[f_ra].d[0]^0x8000000000000000ull,c->v[f_rn].d[0],c->v[f_rm].d[0],64));",
      "" },
    /* ftype=2, o1=0, o0=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=2, o1=0, o0=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=2, o1=1, o0=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=2, o1=1, o0=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, o1=0, o0=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, o1=0, o0=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, o1=1, o0=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* ftype=3, o1=1, o0=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
};

/* FCVT_INT: specialized by sf, ftype, rmode, fop. */
static const SnxA64Case cases_FCVT_INT[] = {
    /* sf=0, ftype=0, rmode=0, fop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,32,0,0,0));",
      "" },
    /* sf=0, ftype=0, rmode=0, fop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,32,1,0,0));",
      "" },
    /* sf=0, ftype=0, rmode=0, fop=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_from_int(c,RX(f_rn),32,32,0,0));",
      "" },
    /* sf=0, ftype=0, rmode=0, fop=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_from_int(c,RX(f_rn),32,32,1,0));",
      "" },
    /* sf=0, ftype=0, rmode=0, fop=4 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,32,0,0,4));",
      "" },
    /* sf=0, ftype=0, rmode=0, fop=5 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,32,1,0,4));",
      "" },
    /* sf=0, ftype=0, rmode=0, fop=6 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(c->v[f_rn].s[0]));",
      "" },
    /* sf=0, ftype=0, rmode=0, fop=7 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,(u32)RX(f_rn));",
      "" },
    /* sf=0, ftype=0, rmode=1, fop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,32,0,0,1));",
      "" },
    /* sf=0, ftype=0, rmode=1, fop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,32,1,0,1));",
      "" },
    /* sf=0, ftype=0, rmode=1, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=1, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=1, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=1, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=1, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=1, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=2, fop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,32,0,0,2));",
      "" },
    /* sf=0, ftype=0, rmode=2, fop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,32,1,0,2));",
      "" },
    /* sf=0, ftype=0, rmode=2, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=2, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=2, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=2, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=2, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=2, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=3, fop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,32,0,0,3));",
      "" },
    /* sf=0, ftype=0, rmode=3, fop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,32,1,0,3));",
      "" },
    /* sf=0, ftype=0, rmode=3, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=3, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=3, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=3, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=3, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=3, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=0, fop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,32,0,0,0));",
      "" },
    /* sf=0, ftype=1, rmode=0, fop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,32,1,0,0));",
      "" },
    /* sf=0, ftype=1, rmode=0, fop=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_from_int(c,RX(f_rn),32,64,0,0));",
      "" },
    /* sf=0, ftype=1, rmode=0, fop=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_from_int(c,RX(f_rn),32,64,1,0));",
      "" },
    /* sf=0, ftype=1, rmode=0, fop=4 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,32,0,0,4));",
      "" },
    /* sf=0, ftype=1, rmode=0, fop=5 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,32,1,0,4));",
      "" },
    /* sf=0, ftype=1, rmode=0, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=0, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=1, fop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,32,0,0,1));",
      "" },
    /* sf=0, ftype=1, rmode=1, fop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,32,1,0,1));",
      "" },
    /* sf=0, ftype=1, rmode=1, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=1, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=1, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=1, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=1, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=1, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=2, fop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,32,0,0,2));",
      "" },
    /* sf=0, ftype=1, rmode=2, fop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,32,1,0,2));",
      "" },
    /* sf=0, ftype=1, rmode=2, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=2, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=2, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=2, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=2, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=2, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=3, fop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,32,0,0,3));",
      "" },
    /* sf=0, ftype=1, rmode=3, fop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,32,1,0,3));",
      "" },
    /* sf=0, ftype=1, rmode=3, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=3, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=3, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=3, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=3, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=3, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=0, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=0, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=0, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=0, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=0, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=0, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=0, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=0, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=1, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=1, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=1, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=1, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=1, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=1, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=1, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=1, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=2, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=2, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=2, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=2, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=2, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=2, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=2, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=2, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=3, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=3, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=3, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=3, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=3, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=3, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=3, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=3, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=0, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=0, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=0, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=0, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=0, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=0, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=0, fop=6 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(c->v[f_rn].h[0]));",
      "" },
    /* sf=0, ftype=3, rmode=0, fop=7 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETHB(f_rd,(u32)RX(f_rn));",
      "" },
    /* sf=0, ftype=3, rmode=1, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=1, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=1, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=1, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=1, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=1, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=1, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=1, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=2, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=2, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=2, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=2, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=2, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=2, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=2, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=2, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=3, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=3, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=3, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=3, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=3, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=3, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=3, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=0, ftype=3, rmode=3, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=0, fop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,64,0,0,0));",
      "" },
    /* sf=1, ftype=0, rmode=0, fop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,64,1,0,0));",
      "" },
    /* sf=1, ftype=0, rmode=0, fop=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_from_int(c,RX(f_rn),64,32,0,0));",
      "" },
    /* sf=1, ftype=0, rmode=0, fop=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETSB(f_rd,snx_fp_from_int(c,RX(f_rn),64,32,1,0));",
      "" },
    /* sf=1, ftype=0, rmode=0, fop=4 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,64,0,0,4));",
      "" },
    /* sf=1, ftype=0, rmode=0, fop=5 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,64,1,0,4));",
      "" },
    /* sf=1, ftype=0, rmode=0, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=0, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=1, fop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,64,0,0,1));",
      "" },
    /* sf=1, ftype=0, rmode=1, fop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,64,1,0,1));",
      "" },
    /* sf=1, ftype=0, rmode=1, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=1, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=1, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=1, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=1, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=1, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=2, fop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,64,0,0,2));",
      "" },
    /* sf=1, ftype=0, rmode=2, fop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,64,1,0,2));",
      "" },
    /* sf=1, ftype=0, rmode=2, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=2, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=2, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=2, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=2, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=2, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=3, fop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,64,0,0,3));",
      "" },
    /* sf=1, ftype=0, rmode=3, fop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,64,1,0,3));",
      "" },
    /* sf=1, ftype=0, rmode=3, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=3, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=3, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=3, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=3, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=3, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=0, fop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,64,0,0,0));",
      "" },
    /* sf=1, ftype=1, rmode=0, fop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,64,1,0,0));",
      "" },
    /* sf=1, ftype=1, rmode=0, fop=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_from_int(c,RX(f_rn),64,64,0,0));",
      "" },
    /* sf=1, ftype=1, rmode=0, fop=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,snx_fp_from_int(c,RX(f_rn),64,64,1,0));",
      "" },
    /* sf=1, ftype=1, rmode=0, fop=4 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,64,0,0,4));",
      "" },
    /* sf=1, ftype=1, rmode=0, fop=5 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,64,1,0,4));",
      "" },
    /* sf=1, ftype=1, rmode=0, fop=6 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(c->v[f_rn].d[0]));",
      "" },
    /* sf=1, ftype=1, rmode=0, fop=7 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETDB(f_rd,RX(f_rn));",
      "" },
    /* sf=1, ftype=1, rmode=1, fop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,64,0,0,1));",
      "" },
    /* sf=1, ftype=1, rmode=1, fop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,64,1,0,1));",
      "" },
    /* sf=1, ftype=1, rmode=1, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=1, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=1, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=1, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=1, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=1, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=2, fop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,64,0,0,2));",
      "" },
    /* sf=1, ftype=1, rmode=2, fop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,64,1,0,2));",
      "" },
    /* sf=1, ftype=1, rmode=2, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=2, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=2, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=2, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=2, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=2, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=3, fop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,64,0,0,3));",
      "" },
    /* sf=1, ftype=1, rmode=3, fop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,64,1,0,3));",
      "" },
    /* sf=1, ftype=1, rmode=3, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=3, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=3, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=3, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=3, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=3, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=0, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=0, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=0, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=0, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=0, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=0, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=0, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=0, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=1, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=1, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=1, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=1, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=1, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=1, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=1, fop=6 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(c->v[f_rn].d[1]));",
      "" },
    /* sf=1, ftype=2, rmode=1, fop=7 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "c->v[f_rd].d[1]=RX(f_rn);",
      "" },
    /* sf=1, ftype=2, rmode=2, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=2, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=2, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=2, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=2, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=2, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=2, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=2, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=3, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=3, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=3, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=3, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=3, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=3, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=3, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=3, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=0, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=0, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=0, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=0, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=0, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=0, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=0, fop=6 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETX(f_rd,(u64)(c->v[f_rn].h[0]));",
      "" },
    /* sf=1, ftype=3, rmode=0, fop=7 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "SETHB(f_rd,(u32)RX(f_rn));",
      "" },
    /* sf=1, ftype=3, rmode=1, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=1, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=1, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=1, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=1, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=1, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=1, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=1, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=2, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=2, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=2, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=2, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=2, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=2, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=2, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=2, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=3, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=3, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=3, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=3, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=3, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=3, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=3, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
    /* sf=1, ftype=3, rmode=3, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; ",
      "",
      "" },
};

/* FCVT_FIX: specialized by sf, ftype, rmode, fop. */
static const SnxA64Case cases_FCVT_FIX[] = {
    /* sf=0, ftype=0, rmode=0, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=0, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=0, fop=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; const u32 f_scale=(w>>10)&63; ",
      "SETSB(f_rd,snx_fp_from_int(c,RX(f_rn),32,32,0,64-f_scale));",
      "" },
    /* sf=0, ftype=0, rmode=0, fop=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; const u32 f_scale=(w>>10)&63; ",
      "SETSB(f_rd,snx_fp_from_int(c,RX(f_rn),32,32,1,64-f_scale));",
      "" },
    /* sf=0, ftype=0, rmode=0, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=0, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=0, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=0, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=1, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=1, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=1, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=1, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=1, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=1, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=1, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=1, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=2, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=2, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=2, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=2, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=2, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=2, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=2, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=2, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=3, fop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; const u32 f_scale=(w>>10)&63; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,32,0,64-f_scale,3));",
      "" },
    /* sf=0, ftype=0, rmode=3, fop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; const u32 f_scale=(w>>10)&63; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,32,1,64-f_scale,3));",
      "" },
    /* sf=0, ftype=0, rmode=3, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=3, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=3, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=3, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=3, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=0, rmode=3, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=0, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=0, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=0, fop=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; const u32 f_scale=(w>>10)&63; ",
      "SETDB(f_rd,snx_fp_from_int(c,RX(f_rn),32,64,0,64-f_scale));",
      "" },
    /* sf=0, ftype=1, rmode=0, fop=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; const u32 f_scale=(w>>10)&63; ",
      "SETDB(f_rd,snx_fp_from_int(c,RX(f_rn),32,64,1,64-f_scale));",
      "" },
    /* sf=0, ftype=1, rmode=0, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=0, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=0, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=0, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=1, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=1, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=1, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=1, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=1, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=1, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=1, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=1, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=2, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=2, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=2, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=2, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=2, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=2, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=2, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=2, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=3, fop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; const u32 f_scale=(w>>10)&63; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,32,0,64-f_scale,3));",
      "" },
    /* sf=0, ftype=1, rmode=3, fop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; const u32 f_scale=(w>>10)&63; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,32,1,64-f_scale,3));",
      "" },
    /* sf=0, ftype=1, rmode=3, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=3, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=3, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=3, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=3, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=1, rmode=3, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=0, ftype=2, rmode=0, fop=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=0, fop=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=0, fop=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=0, fop=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=0, fop=4 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=0, fop=5 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=0, fop=6 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=0, fop=7 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=1, fop=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=1, fop=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=1, fop=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=1, fop=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=1, fop=4 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=1, fop=5 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=1, fop=6 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=1, fop=7 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=2, fop=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=2, fop=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=2, fop=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=2, fop=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=2, fop=4 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=2, fop=5 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=2, fop=6 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=2, fop=7 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=3, fop=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=3, fop=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=3, fop=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=3, fop=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=3, fop=4 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=3, fop=5 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=3, fop=6 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=2, rmode=3, fop=7 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=0, fop=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=0, fop=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=0, fop=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=0, fop=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=0, fop=4 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=0, fop=5 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=0, fop=6 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=0, fop=7 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=1, fop=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=1, fop=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=1, fop=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=1, fop=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=1, fop=4 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=1, fop=5 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=1, fop=6 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=1, fop=7 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=2, fop=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=2, fop=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=2, fop=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=2, fop=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=2, fop=4 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=2, fop=5 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=2, fop=6 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=2, fop=7 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=3, fop=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=3, fop=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=3, fop=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=3, fop=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=3, fop=4 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=3, fop=5 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=3, fop=6 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=0, ftype=3, rmode=3, fop=7 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=0, rmode=0, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=0, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=0, fop=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; const u32 f_scale=(w>>10)&63; ",
      "SETSB(f_rd,snx_fp_from_int(c,RX(f_rn),64,32,0,64-f_scale));",
      "" },
    /* sf=1, ftype=0, rmode=0, fop=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; const u32 f_scale=(w>>10)&63; ",
      "SETSB(f_rd,snx_fp_from_int(c,RX(f_rn),64,32,1,64-f_scale));",
      "" },
    /* sf=1, ftype=0, rmode=0, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=0, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=0, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=0, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=1, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=1, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=1, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=1, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=1, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=1, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=1, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=1, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=2, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=2, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=2, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=2, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=2, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=2, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=2, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=2, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=3, fop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; const u32 f_scale=(w>>10)&63; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,64,0,64-f_scale,3));",
      "" },
    /* sf=1, ftype=0, rmode=3, fop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; const u32 f_scale=(w>>10)&63; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].s[0],32,64,1,64-f_scale,3));",
      "" },
    /* sf=1, ftype=0, rmode=3, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=3, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=3, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=3, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=3, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=0, rmode=3, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=0, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=0, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=0, fop=2 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; const u32 f_scale=(w>>10)&63; ",
      "SETDB(f_rd,snx_fp_from_int(c,RX(f_rn),64,64,0,64-f_scale));",
      "" },
    /* sf=1, ftype=1, rmode=0, fop=3 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; const u32 f_scale=(w>>10)&63; ",
      "SETDB(f_rd,snx_fp_from_int(c,RX(f_rn),64,64,1,64-f_scale));",
      "" },
    /* sf=1, ftype=1, rmode=0, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=0, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=0, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=0, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=1, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=1, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=1, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=1, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=1, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=1, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=1, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=1, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=2, fop=0 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=2, fop=1 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=2, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=2, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=2, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=2, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=2, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=2, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=3, fop=0 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; const u32 f_scale=(w>>10)&63; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,64,0,64-f_scale,3));",
      "" },
    /* sf=1, ftype=1, rmode=3, fop=1 */
    { SNX_FLOW_PLAIN,
      "const u32 f_rd=(w>>0)&31; const u32 f_rn=(w>>5)&31; const u32 f_scale=(w>>10)&63; ",
      "SETX(f_rd,snx_fp_to_int(c,c->v[f_rn].d[0],64,64,1,64-f_scale,3));",
      "" },
    /* sf=1, ftype=1, rmode=3, fop=2 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=3, fop=3 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=3, fop=4 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=3, fop=5 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=3, fop=6 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=1, rmode=3, fop=7 */
    { SNX_FLOW_UNDEF,
      "const u32 f_scale=(w>>10)&63; ",
      "",
      "" },
    /* sf=1, ftype=2, rmode=0, fop=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=0, fop=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=0, fop=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=0, fop=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=0, fop=4 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=0, fop=5 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=0, fop=6 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=0, fop=7 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=1, fop=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=1, fop=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=1, fop=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=1, fop=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=1, fop=4 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=1, fop=5 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=1, fop=6 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=1, fop=7 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=2, fop=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=2, fop=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=2, fop=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=2, fop=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=2, fop=4 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=2, fop=5 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=2, fop=6 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=2, fop=7 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=3, fop=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=3, fop=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=3, fop=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=3, fop=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=3, fop=4 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=3, fop=5 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=3, fop=6 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=2, rmode=3, fop=7 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=0, fop=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=0, fop=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=0, fop=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=0, fop=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=0, fop=4 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=0, fop=5 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=0, fop=6 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=0, fop=7 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=1, fop=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=1, fop=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=1, fop=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=1, fop=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=1, fop=4 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=1, fop=5 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=1, fop=6 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=1, fop=7 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=2, fop=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=2, fop=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=2, fop=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=2, fop=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=2, fop=4 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=2, fop=5 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=2, fop=6 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=2, fop=7 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=3, fop=0 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=3, fop=1 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=3, fop=2 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=3, fop=3 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=3, fop=4 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=3, fop=5 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=3, fop=6 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
    /* sf=1, ftype=3, rmode=3, fop=7 */
    { SNX_FLOW_UNDEF,
      "",
      "",
      "" },
};

/* SIMD_LDST: specialized by no fields. */
static const SnxA64Case cases_SIMD_LDST[] = {
    { SNX_FLOW_PLAIN,
      "",
      "if(!snx_simd(c,M,w)) UNDEF();",
      "" },
};

/* SIMD_LDST1: specialized by no fields. */
static const SnxA64Case cases_SIMD_LDST1[] = {
    { SNX_FLOW_PLAIN,
      "",
      "if(!snx_simd(c,M,w)) UNDEF();",
      "" },
};

/* SIMD_DP: specialized by no fields. */
static const SnxA64Case cases_SIMD_DP[] = {
    { SNX_FLOW_PLAIN,
      "",
      "if(!snx_simd(c,M,w)) UNDEF();",
      "" },
};

/* SIMD_DP2: specialized by no fields. */
static const SnxA64Case cases_SIMD_DP2[] = {
    { SNX_FLOW_PLAIN,
      "",
      "if(!snx_simd(c,M,w)) UNDEF();",
      "" },
};

static const SnxA64Pattern patterns[] = {
    { "ADR", 0x9F000000UL, 0x10000000UL, 0, 0, { { 0, 0 } },
      "0", cases_ADR, sizeof(cases_ADR) / sizeof(cases_ADR[0]) },
    { "ADRP", 0x9F000000UL, 0x90000000UL, 0, 0, { { 0, 0 } },
      "0", cases_ADRP, sizeof(cases_ADRP) / sizeof(cases_ADRP[0]) },
    { "ADDSUB_IMM", 0x1F800000UL, 0x11000000UL, 0, 4, { { 31, 1 }, { 30, 1 }, { 29, 1 }, { 22, 1 } },
      "(((w>>22)&1)<<0)|(((w>>29)&1)<<1)|(((w>>30)&1)<<2)|(((w>>31)&1)<<3)", cases_ADDSUB_IMM, sizeof(cases_ADDSUB_IMM) / sizeof(cases_ADDSUB_IMM[0]) },
    { "LOGIC_IMM", 0x1F800000UL, 0x12000000UL, 1, 2, { { 31, 1 }, { 29, 2 } },
      "(((w>>29)&3)<<0)|(((w>>31)&1)<<2)", cases_LOGIC_IMM, sizeof(cases_LOGIC_IMM) / sizeof(cases_LOGIC_IMM[0]) },
    { "MOVW", 0x1F800000UL, 0x12800000UL, 2, 2, { { 31, 1 }, { 29, 2 } },
      "(((w>>29)&3)<<0)|(((w>>31)&1)<<2)", cases_MOVW, sizeof(cases_MOVW) / sizeof(cases_MOVW[0]) },
    { "BITFIELD", 0x1F800000UL, 0x13000000UL, 3, 2, { { 31, 1 }, { 29, 2 } },
      "(((w>>29)&3)<<0)|(((w>>31)&1)<<2)", cases_BITFIELD, sizeof(cases_BITFIELD) / sizeof(cases_BITFIELD[0]) },
    { "EXTR", 0x7FA00000UL, 0x13800000UL, 4, 1, { { 31, 1 } },
      "(((w>>31)&1)<<0)", cases_EXTR, sizeof(cases_EXTR) / sizeof(cases_EXTR[0]) },
    { "B", 0xFC000000UL, 0x14000000UL, 0, 0, { { 0, 0 } },
      "0", cases_B, sizeof(cases_B) / sizeof(cases_B[0]) },
    { "BL", 0xFC000000UL, 0x94000000UL, 0, 0, { { 0, 0 } },
      "0", cases_BL, sizeof(cases_BL) / sizeof(cases_BL[0]) },
    { "BCOND", 0xFF000010UL, 0x54000000UL, 0, 0, { { 0, 0 } },
      "0", cases_BCOND, sizeof(cases_BCOND) / sizeof(cases_BCOND[0]) },
    { "CBZ", 0x7E000000UL, 0x34000000UL, 0, 2, { { 31, 1 }, { 24, 1 } },
      "(((w>>24)&1)<<0)|(((w>>31)&1)<<1)", cases_CBZ, sizeof(cases_CBZ) / sizeof(cases_CBZ[0]) },
    { "TBZ", 0x7E000000UL, 0x36000000UL, 0, 1, { { 24, 1 } },
      "(((w>>24)&1)<<0)", cases_TBZ, sizeof(cases_TBZ) / sizeof(cases_TBZ[0]) },
    { "BR", 0xFFFFFC1FUL, 0xD61F0000UL, 0, 0, { { 0, 0 } },
      "0", cases_BR, sizeof(cases_BR) / sizeof(cases_BR[0]) },
    { "BLR", 0xFFFFFC1FUL, 0xD63F0000UL, 0, 0, { { 0, 0 } },
      "0", cases_BLR, sizeof(cases_BLR) / sizeof(cases_BLR[0]) },
    { "RET", 0xFFFFFC1FUL, 0xD65F0000UL, 0, 0, { { 0, 0 } },
      "0", cases_RET, sizeof(cases_RET) / sizeof(cases_RET[0]) },
    { "SVC", 0xFFE0001FUL, 0xD4000001UL, 0, 0, { { 0, 0 } },
      "0", cases_SVC, sizeof(cases_SVC) / sizeof(cases_SVC[0]) },
    { "BRK", 0xFFE0001FUL, 0xD4200000UL, 0, 0, { { 0, 0 } },
      "0", cases_BRK, sizeof(cases_BRK) / sizeof(cases_BRK[0]) },
    { "HINT", 0xFFFFF01FUL, 0xD503201FUL, 0, 0, { { 0, 0 } },
      "0", cases_HINT, sizeof(cases_HINT) / sizeof(cases_HINT[0]) },
    { "CLREX", 0xFFFFF0FFUL, 0xD503305FUL, 0, 0, { { 0, 0 } },
      "0", cases_CLREX, sizeof(cases_CLREX) / sizeof(cases_CLREX[0]) },
    { "BARRIER", 0xFFFFF09FUL, 0xD503309FUL, 0, 0, { { 0, 0 } },
      "0", cases_BARRIER, sizeof(cases_BARRIER) / sizeof(cases_BARRIER[0]) },
    { "MSR_IMM", 0xFFF8F01FUL, 0xD500401FUL, 0, 0, { { 0, 0 } },
      "0", cases_MSR_IMM, sizeof(cases_MSR_IMM) / sizeof(cases_MSR_IMM[0]) },
    { "SYS", 0xFFF80000UL, 0xD5080000UL, 0, 0, { { 0, 0 } },
      "0", cases_SYS, sizeof(cases_SYS) / sizeof(cases_SYS[0]) },
    { "MRS", 0xFFF00000UL, 0xD5300000UL, 0, 0, { { 0, 0 } },
      "0", cases_MRS, sizeof(cases_MRS) / sizeof(cases_MRS[0]) },
    { "MSR", 0xFFF00000UL, 0xD5100000UL, 0, 0, { { 0, 0 } },
      "0", cases_MSR, sizeof(cases_MSR) / sizeof(cases_MSR[0]) },
    { "LDST_UIMM", 0x3B000000UL, 0x39000000UL, 0, 3, { { 30, 2 }, { 26, 1 }, { 22, 2 } },
      "(((w>>22)&3)<<0)|(((w>>26)&1)<<2)|(((w>>30)&3)<<3)", cases_LDST_UIMM, sizeof(cases_LDST_UIMM) / sizeof(cases_LDST_UIMM[0]) },
    { "LDST_IMM9", 0x3B200000UL, 0x38000000UL, 0, 4, { { 30, 2 }, { 26, 1 }, { 22, 2 }, { 10, 2 } },
      "(((w>>10)&3)<<0)|(((w>>22)&3)<<2)|(((w>>26)&1)<<4)|(((w>>30)&3)<<5)", cases_LDST_IMM9, sizeof(cases_LDST_IMM9) / sizeof(cases_LDST_IMM9[0]) },
    { "LDST_REG", 0x3B200C00UL, 0x38200800UL, 5, 3, { { 30, 2 }, { 26, 1 }, { 22, 2 } },
      "(((w>>22)&3)<<0)|(((w>>26)&1)<<2)|(((w>>30)&3)<<3)", cases_LDST_REG, sizeof(cases_LDST_REG) / sizeof(cases_LDST_REG[0]) },
    { "LDR_LIT", 0x3B000000UL, 0x18000000UL, 0, 2, { { 30, 2 }, { 26, 1 } },
      "(((w>>26)&1)<<0)|(((w>>30)&3)<<1)", cases_LDR_LIT, sizeof(cases_LDR_LIT) / sizeof(cases_LDR_LIT[0]) },
    { "LDSTP", 0x3A000000UL, 0x28000000UL, 0, 4, { { 30, 2 }, { 26, 1 }, { 23, 2 }, { 22, 1 } },
      "(((w>>22)&1)<<0)|(((w>>23)&3)<<1)|(((w>>26)&1)<<3)|(((w>>30)&3)<<4)", cases_LDSTP, sizeof(cases_LDSTP) / sizeof(cases_LDSTP[0]) },
    { "LDSTX", 0x3F000000UL, 0x08000000UL, 0, 4, { { 30, 2 }, { 23, 1 }, { 22, 1 }, { 21, 1 } },
      "(((w>>21)&1)<<0)|(((w>>22)&1)<<1)|(((w>>23)&1)<<2)|(((w>>30)&3)<<3)", cases_LDSTX, sizeof(cases_LDSTX) / sizeof(cases_LDSTX[0]) },
    { "LOGIC_REG", 0x1F000000UL, 0x0A000000UL, 6, 3, { { 31, 1 }, { 29, 2 }, { 21, 1 } },
      "(((w>>21)&1)<<0)|(((w>>29)&3)<<1)|(((w>>31)&1)<<3)", cases_LOGIC_REG, sizeof(cases_LOGIC_REG) / sizeof(cases_LOGIC_REG[0]) },
    { "ADDSUB_SREG", 0x1F200000UL, 0x0B000000UL, 7, 3, { { 31, 1 }, { 30, 1 }, { 29, 1 } },
      "(((w>>29)&1)<<0)|(((w>>30)&1)<<1)|(((w>>31)&1)<<2)", cases_ADDSUB_SREG, sizeof(cases_ADDSUB_SREG) / sizeof(cases_ADDSUB_SREG[0]) },
    { "ADDSUB_EXT", 0x1FE00000UL, 0x0B200000UL, 8, 3, { { 31, 1 }, { 30, 1 }, { 29, 1 } },
      "(((w>>29)&1)<<0)|(((w>>30)&1)<<1)|(((w>>31)&1)<<2)", cases_ADDSUB_EXT, sizeof(cases_ADDSUB_EXT) / sizeof(cases_ADDSUB_EXT[0]) },
    { "ADC", 0x1FE0FC00UL, 0x1A000000UL, 0, 3, { { 31, 1 }, { 30, 1 }, { 29, 1 } },
      "(((w>>29)&1)<<0)|(((w>>30)&1)<<1)|(((w>>31)&1)<<2)", cases_ADC, sizeof(cases_ADC) / sizeof(cases_ADC[0]) },
    { "CCMP", 0x3FE00410UL, 0x3A400000UL, 0, 3, { { 31, 1 }, { 30, 1 }, { 11, 1 } },
      "(((w>>11)&1)<<0)|(((w>>30)&1)<<1)|(((w>>31)&1)<<2)", cases_CCMP, sizeof(cases_CCMP) / sizeof(cases_CCMP[0]) },
    { "CSEL", 0x3FE00800UL, 0x1A800000UL, 0, 3, { { 31, 1 }, { 30, 1 }, { 10, 1 } },
      "(((w>>10)&1)<<0)|(((w>>30)&1)<<1)|(((w>>31)&1)<<2)", cases_CSEL, sizeof(cases_CSEL) / sizeof(cases_CSEL[0]) },
    { "DP1", 0x7FFF0000UL, 0x5AC00000UL, 0, 2, { { 31, 1 }, { 10, 6 } },
      "(((w>>10)&63)<<0)|(((w>>31)&1)<<6)", cases_DP1, sizeof(cases_DP1) / sizeof(cases_DP1[0]) },
    { "DP2", 0x7FE00000UL, 0x1AC00000UL, 0, 2, { { 31, 1 }, { 10, 6 } },
      "(((w>>10)&63)<<0)|(((w>>31)&1)<<6)", cases_DP2, sizeof(cases_DP2) / sizeof(cases_DP2[0]) },
    { "DP3", 0x7F000000UL, 0x1B000000UL, 0, 3, { { 31, 1 }, { 21, 3 }, { 15, 1 } },
      "(((w>>15)&1)<<0)|(((w>>21)&7)<<1)|(((w>>31)&1)<<4)", cases_DP3, sizeof(cases_DP3) / sizeof(cases_DP3[0]) },
    { "FP2", 0xFF200C00UL, 0x1E200800UL, 0, 2, { { 22, 2 }, { 12, 4 } },
      "(((w>>12)&15)<<0)|(((w>>22)&3)<<4)", cases_FP2, sizeof(cases_FP2) / sizeof(cases_FP2[0]) },
    { "FP1", 0xFF207C00UL, 0x1E204000UL, 0, 2, { { 22, 2 }, { 15, 6 } },
      "(((w>>15)&63)<<0)|(((w>>22)&3)<<6)", cases_FP1, sizeof(cases_FP1) / sizeof(cases_FP1[0]) },
    { "FCMP", 0xFF20FC07UL, 0x1E202000UL, 0, 2, { { 22, 2 }, { 3, 2 } },
      "(((w>>3)&3)<<0)|(((w>>22)&3)<<2)", cases_FCMP, sizeof(cases_FCMP) / sizeof(cases_FCMP[0]) },
    { "FCCMP", 0xFF200C00UL, 0x1E200400UL, 0, 1, { { 22, 2 } },
      "(((w>>22)&3)<<0)", cases_FCCMP, sizeof(cases_FCCMP) / sizeof(cases_FCCMP[0]) },
    { "FCSEL", 0xFF200C00UL, 0x1E200C00UL, 0, 1, { { 22, 2 } },
      "(((w>>22)&3)<<0)", cases_FCSEL, sizeof(cases_FCSEL) / sizeof(cases_FCSEL[0]) },
    { "FMOV_IMM", 0xFF201FE0UL, 0x1E201000UL, 0, 1, { { 22, 2 } },
      "(((w>>22)&3)<<0)", cases_FMOV_IMM, sizeof(cases_FMOV_IMM) / sizeof(cases_FMOV_IMM[0]) },
    { "FP3", 0xFF000000UL, 0x1F000000UL, 0, 3, { { 22, 2 }, { 21, 1 }, { 15, 1 } },
      "(((w>>15)&1)<<0)|(((w>>21)&1)<<1)|(((w>>22)&3)<<2)", cases_FP3, sizeof(cases_FP3) / sizeof(cases_FP3[0]) },
    { "FCVT_INT", 0x7F20FC00UL, 0x1E200000UL, 10, 4, { { 31, 1 }, { 22, 2 }, { 19, 2 }, { 16, 3 } },
      "(((w>>16)&7)<<0)|(((w>>19)&3)<<3)|(((w>>22)&3)<<5)|(((w>>31)&1)<<7)", cases_FCVT_INT, sizeof(cases_FCVT_INT) / sizeof(cases_FCVT_INT[0]) },
    { "FCVT_FIX", 0x7F200000UL, 0x1E000000UL, 9, 4, { { 31, 1 }, { 22, 2 }, { 19, 2 }, { 16, 3 } },
      "(((w>>16)&7)<<0)|(((w>>19)&3)<<3)|(((w>>22)&3)<<5)|(((w>>31)&1)<<7)", cases_FCVT_FIX, sizeof(cases_FCVT_FIX) / sizeof(cases_FCVT_FIX[0]) },
    { "SIMD_LDST", 0xBF000000UL, 0x0C000000UL, 0, 0, { { 0, 0 } },
      "0", cases_SIMD_LDST, sizeof(cases_SIMD_LDST) / sizeof(cases_SIMD_LDST[0]) },
    { "SIMD_LDST1", 0xBF000000UL, 0x0D000000UL, 0, 0, { { 0, 0 } },
      "0", cases_SIMD_LDST1, sizeof(cases_SIMD_LDST1) / sizeof(cases_SIMD_LDST1[0]) },
    { "SIMD_DP", 0x9E000000UL, 0x0E000000UL, 0, 0, { { 0, 0 } },
      "0", cases_SIMD_DP, sizeof(cases_SIMD_DP) / sizeof(cases_SIMD_DP[0]) },
    { "SIMD_DP2", 0xDE000000UL, 0x5E000000UL, 0, 0, { { 0, 0 } },
      "0", cases_SIMD_DP2, sizeof(cases_SIMD_DP2) / sizeof(cases_SIMD_DP2[0]) },
};
