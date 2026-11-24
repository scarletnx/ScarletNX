/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The AdvSIMD part of the arm64 back end (arm64_codegen.c includes this after mach_codegen.inc): the guest's Advanced
   SIMD and floating-point data processing as its own words (vec), and the vector helpers the SIMD&FP structures'
   loads and stores take (a register whole, an element in and out of a general-purpose register, an element
   duplicated: UMOV, INS, DUP), on the region's SSA in V registers. vec() returns 1 made, 0 not one of these (the
   function goes to another back end), -1 UNDEF */

#define X sgx(g)

/* V[n] as a vector register */
static int vread(SgCtx* g, unsigned n) {
    SgD d = sg__V_read_bits(g, n, 127, 0);
    return sgx_xreg(X, &d);
}
/* V[d] = r, its high doubleword cleared where q is 0 */
static void vwrite(SgCtx* g, unsigned d, int r, int q) {
    if (!q) r = sgx_xclear(X, r, 64);
    sg__V_write(g, d, sgx_xval(128, r));
}
static int vzero(SgCtx* g) { return sgx_xconst(X, 0, 0); }

/* an element's imm5: its size (0 B ... 3 D) and index */
static SnxU32 v_imm5(unsigned size, int i) { return (SnxU32)i << (size + 1) | 1u << size; }
/* UMOV Wd or Xd, Vn.<T>[imm]: imm packs size << 8 | index */
SGA_VE(umov) {
    unsigned size = (unsigned)(imm >> 8) & 3;
    (void)cc;
    sga_w(b, sge_a64_umov_advsimd(r[0], r[1], v_imm5(size, (int)(imm & 0xFF)), size == 3));
}
/* INS Vd.<T>[imm], Wn or Xn (tied) */
SGA_VE(ins) {
    (void)cc;
    sga_w(b, sge_a64_ins_advsimd_gen(r[0], r[1], v_imm5((unsigned)(imm >> 8) & 3, (int)(imm & 0xFF))));
}
/* DUP Vd.<T>, Vn.<T>[imm], all 128 bits */
SGA_VE(dupe) {
    (void)cc;
    sga_w(b, sge_a64_dup_advsimd_elt_simd(r[0], r[1], v_imm5((unsigned)(imm >> 8) & 3, (int)(imm & 0xFF)), 1));
}
static const SgXVForm sga_v_umov_f = {sga_v_umov, 0, 2, 0, 1, 2, 2, 0, NULL};
static const SgXVForm sga_v_ins_f = {sga_v_ins, 0, 2, 0, 1, 2, 1, 0, NULL};
static const SgXVForm sga_v_dupe_f = {sga_v_dupe, 0, 2, 0, 1, 2, 3, 0, NULL};

/* element i of size (0 B ... 3 D) of a, zero extended in a general-purpose register */
static SgD lane_get(SgCtx* g, unsigned size, int a, int i) {
    int in[4] = {0, a, 0, 0};
    int r = sgx_vins(X, &sga_v_umov_f, 0, 0, in, (long long)size << 8 | i, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
    X->v[r].zab = (unsigned char)(8u << size);
    return sgx_val(8u << size, r);
}
/* a with element i of size = v */
static int lane_put(SgCtx* g, unsigned size, int a, int i, SgD v) {
    int in[4] = {a, 0, 0, 0};
    v = sg_d_zext(g, v, 64);
    in[1] = sgx_reg(X, &v);
    return sgx_vins(X, &sga_v_ins_f, 0, 1, in, (long long)size << 8 | i, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
}
/* element e of size of a in every element */
static int vdup(SgCtx* g, unsigned size, int a, int e) {
    int in[4] = {0, a, 0, 0};
    return sgx_vins(X, &sga_v_dupe_f, 0, 0, in, (long long)size << 8 | e, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
}

/* ---- the guest's own instruction ----
   An Advanced SIMD or floating-point data-processing word of the guest is an instruction of the host's: it is
   written again, its V registers' fields (Rd, Rn, Rm, Ra, found in the word's clause of the A64 Sail's encoders,
   a64_enc.inc's sge_a64_table) naming the host registers that hold the guest's. Its Rd is read as well as written
   (an accumulation's, an insertion's, a narrowing's second half: what it doesn't read costs a load at most); the
   rest of the register it leaves as the architecture says. Under the guest's FPCR in the host's, the host's FPSR
   ORed into the guest's after it (SGX_M_FPENTER, SGX_M_FPLEAVE), it computes what the guest's would, its exceptions'
   flags and QC included; FCMP's and FCCMP's flags the guest's NZCV, and FCCMP's and FCSEL's conditions on the guest's
   flags in the host's (flags_into_host). Not taken: a clause of a general-purpose register (UMOV, INS, FMOV, the conversions to and
   from one), or of a feature
   the host lacks (g_host_a64) */

/* the clause of word: the first of the table whose fixed bits it has, -1 for none */
static int vclause(SnxU32 word) {
    unsigned k;
    for (k = 0; k < sge_a64_count; ++k)
        if ((word & sge_a64_table[k].mask) == sge_a64_table[k].value) return (int)k;
    return -1;
}
/* the word with its fields Rd, Rn, Rm, Ra the registers r[0..3] (imm: the word, bit 32 + k where field k is one, and
   bit 35 where Rm is a by-element form's of 4 bits, which V7 stands for) */
SGA_VE(pass) {
    SnxU32 w = (SnxU32)imm;
    (void)cc;
    w = (w & ~0x1Fu) | r[0];
    if (imm >> 32 & 1) w = (w & ~(0x1Fu << 5)) | (SnxU32)r[1] << 5;
    if (imm >> 35 & 1) {
        /* a by-element Rm of 4 bits (V0-V15): the register copied to V7, the scratch no value takes, first */
        sga_w(b, sge_a64_orr_advsimd_reg(7, r[2], r[2], 1));
        w = (w & ~(0xFu << 16)) | 7u << 16;
    } else if (imm >> 33 & 1)
        w = (w & ~(0x1Fu << 16)) | (SnxU32)r[2] << 16;
    if (imm >> 34 & 1) w = (w & ~(0x1Fu << 10)) | (SnxU32)r[3] << 10;
    sga_w(b, w);
}
static const SgXVForm sga_v_pass_f = {sga_v_pass, 0, 4, 0, 1, 2, 15, 0, NULL};
/* FCMP, FCMPE, FCCMP and FCCMPE: the word with Rn and Rm the registers r[0] and r[1] (but FCMP's with zero, imm's bit
   36 and the word's 3); it writes the flags alone */
SGA_VE(pcmp) {
    SnxU32 w = (SnxU32)imm;
    (void)cc;
    w = (w & ~(0x1Fu << 5)) | (SnxU32)r[0] << 5;
    if (!(imm >> 36 & 1) || !(w >> 3 & 1)) w = (w & ~(0x1Fu << 16)) | (SnxU32)r[1] << 16; /* not FCMP with zero */
    sga_w(b, w);
}
static const SgXVForm sga_v_pcmp_f = {sga_v_pcmp, 0, 2, 0, 1, 2, 3, 0, NULL};

/* the host's flags the guest's NZCV: already, where the flags' origin is the last instruction to write the host's
   (a compare just before); else PSTATE's four into bits 31-28 and MSR NZCV (SGX_M_SETNZCV) */
static void flags_into_host(SgCtx* g) {
    SgX* x = X;
    SgD nzcv;
    SgXIns* i;
    int r;
    if (g->origin >= 0 && x->flags_ins == g->origin) return;
    nzcv = sg_d_concat(g, sg_d_concat(g, sg_PSTATE_read(g, SG_PSTATE_N), sg_PSTATE_read(g, SG_PSTATE_Z)),
                       sg_d_concat(g, sg_PSTATE_read(g, SG_PSTATE_C), sg_PSTATE_read(g, SG_PSTATE_V)));
    nzcv = sg_d_lsl(g, sg_d_zext(g, nzcv, 64), 28);
    r = sgx_reg(x, &nzcv);
    sgx_flags_clobbered(x);
    i = sgx_append(x, SGX_K_MACHINE);
    i->op = SGX_M_SETNZCV;
    i->in[0] = r;
    i->fsel = &sgx_sel_cmp_64[0];
    x->flags_ins = x->n - 1;
    g->origin = x->n - 1;
}

/* the host's FPCR the guest's from here: SGX_M_FPENTER, or none where the last floating-point word of the region left
   it so (its SGX_M_FPLEAVE dropped: nothing since calls out, reads FPSR or FPCR, or branches) */
static void fp_enter(SgCtx* g) {
    SgX* x = X;
    int i;
    for (i = x->n - 1; i >= 0; --i) {
        SgXIns* s = &x->ins[i];
        if (s->dead) continue;
        if (s->kind == SGX_K_MACHINE && s->op == SGX_M_FPLEAVE) {
            s->dead = 1;
            return;
        }
        if (s->kind == SGX_K_MACHINE || s->kind == SGX_K_UNDEF || s->kind == SGX_K_COLD || s->kind == SGX_K_RET)
            break;
        if ((s->kind == SGX_K_LOAD || s->kind == SGX_K_STORE) &&
            (s->disp == (int)offsetof(CPU, fpsr) || s->disp == (int)offsetof(CPU, fpcr)))
            break;
    }
    sgx_simple(g, SGX_M_FPENTER);
}

static int vec(Unit* u, const SnxA64Insn* insn, SnxU32 word) {
    SgCtx* g = u->g;
    int k = vclause(word), in[4] = {0, 0, 0, 0}, r;
    unsigned f, rd = word & 31, rn = word >> 5 & 31, rm = word >> 16 & 31, ra = word >> 10 & 31;
    long long imm = word;
    const char* name;
    SgXIns* i;
    if (k < 0 || strcmp(sge_a64_table[k].family, "DataProcFPSIMD") || (insn->feature & ~g_host_a64)) return 0;
    name = sge_a64_table[k].name;
    if ((!strcmp(name, "tbl_advsimd") || !strcmp(name, "tbx_advsimd")) && (word >> 13 & 3)) {
        /* a table of len + 1 registers from Rn, which the host's need not hold in a row: TBL (or TBX, of Vd) of the
           first, then TBX of each next with the index 16 lower a register (an index out of a step's 16 bytes leaves
           those bytes as they are, a lowered one below 16 wrapping past 240) */
        unsigned len = word >> 13 & 3, q = word >> 30 & 1, t;
        int idx = vread(g, rm), acc = name[2] == 'x' ? vread(g, rd) : 0, step[4] = {0, 0, 0, 0};
        for (t = 0; t <= len; ++t) {
            long long w = name[2] == 'x' || t ? sge_a64_tbx_advsimd(0, 0, 0, 0, q) : sge_a64_tbl_advsimd(0, 0, 0, 0, q);
            int ix = idx;
            if (t) {
                SnxU64 k = 0x0101010101010101ull * (16 * t); /* 16t in every byte */
                int sin[4] = {0, idx, sgx_xconst(X, k, k), 0};
                ix = sgx_vins(X, &sga_v_pass_f, 0, 0, sin, (long long)sge_a64_sub_advsimd_simd(0, 0, 0, 0, 1) | 3ll << 32,
                              0, SGX_VM_NONE, 0, 0, 0, 0, 0);
            }
            step[0] = acc ? acc : vzero(g);
            step[1] = vread(g, (rn + t) & 31);
            step[2] = ix;
            acc = sgx_vins(X, &sga_v_pass_f, 0, 1, step, w | 3ll << 32, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
        }
        sg__V_write(g, rd, sgx_xval(128, acc));
        return 1;
    }
    if (!strcmp(name, "fcmp_float") || !strcmp(name, "fcmpe_float") || !strcmp(name, "fccmp_float") ||
        !strcmp(name, "fccmpe_float")) {
        /* the host's NZCV the guest's: PSTATE written from them, and a B.cond after it reads them (origin); FCCMP's
           condition is on the guest's flags, in the host's first */
        int ins, cin[4] = {vread(g, rn), vread(g, rm), 0, 0};
        if (name[2] == 'c') flags_into_host(g);
        else
            imm |= 1ll << 36; /* FCMP's bit 3: the compare with zero (FCCMP's is nzcv's) */
        fp_enter(g);
        sgx_vins(X, &sga_v_pcmp_f, -1, 0, cin, imm, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
        ins = X->flags_ins;
        X->ins[ins].side = 1;
        X->ins[ins].fsel = &sgx_sel_cmp_64[0]; /* the identity of Arm's flags as the host's */
        sgx_simple(g, SGX_M_FPLEAVE);
        sg_PSTATE_write(g, SG_PSTATE_N, 4, sgx_nzcv(X, ins));
        return 1;
    }
    if (strstr(name, "_gen") || strstr(name, "float_int") || strstr(name, "float_fix") || !strncmp(name, "fcmp", 4) ||
        !strncmp(name, "fccmp", 5) || !strncmp(name, "umov", 4) ||
        !strncmp(name, "smov", 4) || !strncmp(name, "fjcvtzs", 7))
        return 0;
    {
        /* a by-element form's Rm of 4 bits is M:Rm, 5 bits, for elements of 32 and 64 bits ("size" 2 or 3, or "sz"
           of a single's or a double's, FCMLA's pairs of halves, a dot product's); of 16 bits (FMLAL's too, whose "sz"
           is 0) M is the index's, and Rm one of V0-V15 */
        int rm4 = 0, has_m = 0, wide = 0;
        for (f = 0; f < sge_a64_table[k].nfields; ++f) {
            const char* fn = sge_a64_table[k].fields[f].name;
            unsigned lo = sge_a64_table[k].fields[f].lo, width = sge_a64_table[k].fields[f].width;
            /* TBL's and TBX's table of len + 1 registers from Rn: consecutive ones, which the host's need not be */
            if (!strcmp(fn, "len") && (word >> lo & 3)) return 0;
            if (!strcmp(fn, "M")) has_m = 1;
            if (!strcmp(fn, "sz") || (!strcmp(fn, "size") && (word >> lo & 3) >= 2)) wide = 1;
            if (strcmp(fn, "Rd") && strcmp(fn, "Rn") && strcmp(fn, "Rm") && strcmp(fn, "Ra")) {
                if (!strncmp(fn, "Rt", 2) || !strcmp(fn, "Rs")) return 0; /* a general-purpose register's */
                continue;
            }
            if (width == 4 && !strcmp(fn, "Rm")) rm4 = 1;
            else if (width != 5)
                return 0;
            if (fn[1] == 'n') imm |= 1ll << 32;
            else if (fn[1] == 'm')
                imm |= 1ll << 33;
            else if (fn[1] == 'a')
                imm |= 1ll << 34;
        }
        if (!strncmp(name, "fcmla", 5) || strstr(name, "dot_")) wide = 1;
        if (!strncmp(name, "fmlal", 5) || !strncmp(name, "fmlsl", 5)) wide = 0;
        if (rm4 && !(has_m && wide)) {
            imm |= 1ll << 35; /* Rm copied to V7 first (sga_v_pass) */
            rm &= 15;         /* bit 20 is M, the index's */
        }
    }
    in[0] = vread(g, rd);
    if (imm >> 32 & 1) in[1] = vread(g, rn);
    if (imm >> 33 & 1) in[2] = vread(g, rm);
    if (imm >> 34 & 1) in[3] = vread(g, ra);
    if (!strcmp(name, "fcsel_float")) flags_into_host(g); /* its condition the guest's flags' */
    fp_enter(g);
    r = sgx_vins(X, &sga_v_pass_f, 0, 1, in, imm, 0, SGX_VM_NONE, 0, 0, 0, 0, 0);
    i = &X->ins[X->v[r].def];
    i->side = 1; /* its FPSR flags, though nothing reads its value */
    sgx_simple(g, SGX_M_FPLEAVE);
    sg__V_write(g, rd, sgx_xval(128, r));
    return 1;
}

#undef X
