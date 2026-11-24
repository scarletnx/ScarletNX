/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An SM86 compute program on a device whose compute subgroups are narrower than a warp's 32 (sm86_lone.c): the idioms
   that a compiler combines a warp's atomics with, each run as a warp of 32 lanes together (the module with subgroups)
   and as 32 lone lanes one after the other (the module without), from the same memory, which must leave the same words;
   and the same programs with one instruction changed, which no lone lane gives what the console does, refused with the
   instruction. The programs are this file's, their shapes a compiler's, no game's */
enum { LL_COUNTER = 0, LL_PLACES = 1, LL_WORDS = 256, LL_BANK = 0x400, LL_AT = 0x100 };

typedef struct {
    u8 code[16 * 160];
    u32 n;
} Lone;
static u32 ll(Lone* p, Word w) {
    put(p->code, p->n, w);
    return p->n++;
}
/* an instruction of two words, as a compiler writes it */
static Word ll_raw(u64 lo, u64 hi) { return (Word){{lo, hi}}; }
static Word ll_if(Word w, u32 pred, int negate) {
    set(&w, 12, 3, pred);
    set(&w, 15, 1, (u64)negate);
    return w;
}
static Word ll_s2r(u32 dst, u32 sr) {
    Word x = fix(0x919);
    set(&x, 16, 8, dst);
    set(&x, 72, 8, sr);
    return x;
}
/* a compiler's ballot of the running lanes into a uniform register */
static Word ll_voteu(u32 ur) {
    Word x = ll_raw(0x0000000000047886ull, 0x000fe400038e0100ull);
    set(&x, 16, 6, ur);
    return x;
}
/* UBREV ur = the bits of ur reversed, UFLO.SH of it: the lowest running lane of a ballot */
static Word ll_ubrev(u32 ur, u32 from) {
    Word x = ll_raw(0x00000007000672beull, 0x000fc80008000000ull);
    set(&x, 16, 6, ur);
    set(&x, 32, 6, from);
    return x;
}
static Word ll_uflo(u32 ur, u32 from, int sh) {
    Word x = ll_raw(0x00000006000672bdull, 0x000fcc00080e0400ull);
    set(&x, 16, 6, ur);
    set(&x, 32, 6, from);
    set(&x, 74, 1, (u64)sh);
    return x;
}
/* POPC and FLO (with SH) of a uniform register into a register */
static Word ll_popc_ur(u32 dst, u32 ur) {
    Word x = ll_raw(0x0000000400157d09ull, 0x000e220008000000ull);
    set(&x, 16, 8, dst);
    set(&x, 32, 6, ur);
    return x;
}
static Word ll_flo_ur(u32 dst, u32 ur, int sh) {
    Word x = ll_raw(0x0000000600037d00ull, 0x000ea200080e0400ull);
    set(&x, 16, 8, dst);
    set(&x, 32, 6, ur);
    set(&x, 74, 1, (u64)sh);
    return x;
}
static Word ll_popc(u32 dst, u32 reg) { /* POPC Rd, Rs */
    Word x = ll_raw(0x0000000400057309ull, 0x000e240008000000ull);
    set(&x, 16, 8, dst);
    set(&x, 32, 8, reg);
    return x;
}
static Word ll_lop3_ur(u32 dst, u32 a, u32 ur) { /* LOP3.AND Rd, Ra, URb */
    Word x = ll_raw(0x0000000704047c12ull, 0x001fe2000f8ec0ffull);
    set(&x, 16, 8, dst);
    set(&x, 24, 8, a);
    set(&x, 32, 6, ur);
    return x;
}
/* ISETP.EQ or .NE of a register and a uniform one, into P0 */
static Word ll_isetp_ur(u32 p, u32 a, u32 ur, int not_equal) {
    Word x = ll_raw(0x0000000502007c0cull, 0x002fda000bf02070ull);
    set(&x, 24, 8, a);
    set(&x, 32, 6, ur);
    set(&x, 76, 3, not_equal ? 5 : 2);
    set(&x, 81, 3, p);
    return x;
}
static Word ll_shfl(u32 dst, u32 value, u32 lane) { /* SHFL.IDX PT, Rd, Rv, Rl, 0x1f */
    Word x = ll_raw(0x00001f0302027589ull, 0x014e2a00000e0000ull);
    set(&x, 16, 8, dst);
    set(&x, 24, 8, value);
    set(&x, 32, 8, lane);
    return x;
}
static Word ll_red(u32 address, u32 data) { /* RED.ADD [Ra], Rdata, the pair at Ra */
    Word x = ll_raw(0x000000030000798eull, 0x0011e2000010e100ull);
    set(&x, 24, 8, address);
    set(&x, 32, 8, data);
    return x;
}
static Word ll_match(u32 address, u32 p) { /* MATCH.ALL P, the pair at Ra, its mask to RZ */
    Word x = ll_raw(0x00000000ff73a1ull, 0x000e240000000200ull);
    set(&x, 16, 8, 255);
    set(&x, 24, 8, address);
    set(&x, 81, 3, p);
    return x;
}
/* the start of each program: R1 = the thread, R2:R3 = the buffer's address, R14 = the lane */
static void ll_start(Lone* p) {
    ll(p, ll_s2r(1, 0x21));
    ll(p, ldc(2, 255, 0, LL_BANK, 5));
    ll(p, ll_s2r(14, 0));
}
/* the end of each: the place of the lane in R9, and its thread stored there */
static void ll_end(Lone* p) {
    ll(p, mov_imm(13, 4));
    Word m = alu(0x024, 1, 10, 9); /* IMAD R10, R9, R13 (4), RZ */
    set(&m, 32, 8, 13);
    set(&m, 64, 8, 255);
    ll(p, m);
    ll(p, iadd3(10, 2, 10));
    ll(p, mov_reg(11, 3));
    ll(p, stg_nou(10, 4 * LL_PLACES, 1));
    ll(p, exit_insn());
    ll(p, bra(p->n, p->n, 7, 0));
}
/* what one program has otherwise than the idiom */
enum {
    LL_AS_IT_IS,
    LL_DATA,       /* the leader adds something other than the count */
    LL_LANE,       /* SHFL reads the lane's own, not the leader */
    LL_VALUE,      /* SHFL reads what is no atomic's old value */
    LL_RANK,       /* the lane's number, not its rank, beside the old value */
    LL_ADDRESS,    /* each lane's leader adds at an address of its own */
    LL_NEGATED,    /* all but the leader add */
    LL_MASK,       /* LEMASK, not LTMASK */
    LL_SEGMENT,    /* SHFL in segments of 4 */
    LL_UP,         /* SHFL.UP */
    LL_REDUX,      /* a reduction over the warp */
    LL_STORE_LANE, /* the lane's number is stored */
    LL_NO_SH,      /* FLO of a reversed ballot with no SH, of the lowest leader */
    LL_RUN_STORE,  /* the leader's run stores */
    LL_RUN_VALUE,  /* what the leader's run wrote is read by every lane */
    LL_RUN_PRED,   /* the leader's run writes the predicate that its branch took */
    LL_VOTE_PRED,  /* the ballot writes a predicate too */
    LL_TABLE,      /* LOP3 of another table than a & b */
    LL_COMPARE,    /* the lane below the leader, not at it */
    LL_MIN,        /* the leader's atomic is a minimum of the count */
    LL_NO_REV,     /* FLO.SH of the ballot as it is */
    LL_OTHER,      /* the count is of a ballot that more lanes took part in */
    LL_OF_PRED,    /* the ballot is of a predicate, not of the running lanes */
    LL_OF_NOT,     /* the ballot is of no lane (!PT) */
    LL_CHANGES
};
/* the combined add, its leader by the predicate that a comparison sets (guard), or by a branch over it, its leader the
   highest running lane or the lowest (lowest), and the ballot's count */
static void ll_combined(Lone* p, int branch, int lowest, int change) {
    ll_start(p);
    if (change == LL_ADDRESS) { /* R2 + 4 * the thread */
        Word m = alu(0x024, 4, 20, 1);
        set(&m, 32, 32, 4);
        set(&m, 64, 8, 255);
        ll(p, m);
        ll(p, iadd3(20, 2, 20));
        ll(p, mov_reg(21, 3));
    }
    if (change == LL_REDUX) {
        Word x = ll_raw(0x00000000010673c4ull, 0x000e220000014000ull); /* REDUX UR6, R1 */
        ll(p, x);
    }
    if (change == LL_OTHER || change == LL_OF_PRED) /* P1: the thread is below R4's */
        ll(p, isetp(1, 7, 1, 4, 1, 0, 0, 0, 0, 7));
    if (change == LL_OTHER) { /* a ballot of every lane, then some are gone */
        ll(p, ll_voteu(9));
        ll(p, ll_popc_ur(24, 9));
        ll(p, ll_if(exit_insn(), 1, 0));
    }
    Word ballot = ll_voteu(7);
    if (change == LL_OF_PRED) set(&ballot, 87, 3, 1);
    if (change == LL_OF_NOT) set(&ballot, 90, 1, 1);
    if (change == LL_VOTE_PRED) set(&ballot, 81, 3, 3);
    ll(p, ballot);
    if (lowest) {
        ll(p, ll_ubrev(6, 7));
        ll(p, ll_uflo(6, 6, change != LL_NO_SH));
    } else
        ll(p, ll_uflo(6, 7, change == LL_NO_REV));
    ll(p, ll_popc_ur(4, 7)); /* the count */
    ll(p, mov_reg(7, 255));
    Word compare = ll_isetp_ur(0, 14, 6, branch);
    if (change == LL_COMPARE) set(&compare, 76, 3, 1);
    ll(p, compare);
    const u32 address = change == LL_ADDRESS ? 20 : 2, data = change == LL_DATA ? 1 : change == LL_OTHER ? 24 : 4;
    Word add = atomg_add(7, address, 0, data);
    if (change == LL_MIN) set(&add, 87, 4, 1);
    if (branch) {
        const u32 b = ll(p, bra(p->n, p->n, 0, 0)); /* to the join, patched */
        if (change == LL_RUN_STORE) ll(p, stg_nou(2, 4 * 200, 1));
        if (change == LL_RUN_VALUE) ll(p, mov_imm(17, 5));
        ll(p, add);
        if (change == LL_RUN_PRED) { /* P0 again, of what is no lane's, then a value of the run */
            ll(p, isetp(0, 7, 1, 1, 2, 0, 0, 0, 0, 7));
            ll(p, mov_imm(17, 5));
        }
        put(p->code, b, bra(b, p->n, 0, 0));
    } else
        ll(p, ll_if(add, 0, change == LL_NEGATED));
    ll(p, ll_voteu(5)); /* the same lanes, after the leader's run: another ballot of them */
    ll(p, ll_s2r(8, change == LL_MASK ? 0x3a : 0x39));
    if (lowest) {
        ll(p, ll_ubrev(5, 5));
        ll(p, ll_flo_ur(15, 5, 1));
    } else
        ll(p, ll_flo_ur(15, 5, 0));
    Word below = ll_lop3_ur(8, 8, 7);
    if (change == LL_TABLE) set(&below, 72, 8, 0xfc); /* a | b */
    ll(p, below);
    ll(p, ll_popc(16, 8));
    Word shfl = ll_shfl(7, change == LL_VALUE ? 1 : 7, change == LL_LANE ? 14 : 15);
    if (change == LL_SEGMENT) set(&shfl, 40, 13, 0x1c1f);
    if (change == LL_UP) set(&shfl, 58, 2, 1);
    ll(p, shfl);
    ll(p, iadd3(9, 7, change == LL_RANK ? 14 : 16));
    if (change == LL_STORE_LANE) ll(p, stg_nou(2, 4 * 201, 14));
    if (change == LL_RUN_VALUE || change == LL_RUN_PRED) ll(p, stg_nou(2, 4 * 202, 17));
    ll_end(p);
}

/* MATCH.ALL of the atomic's address: the warp's one address, whose leader adds the count, or each lane's own add of 1.
   groups addresses a warp, and whether the old value is taken (returns), or the program ends */
enum {
    LM_AS_IT_IS,
    LM_TWO,             /* each lane's own adds 2 */
    LM_ANY,             /* MATCH.ANY */
    LM_SINGLE,          /* MATCH.ALL of the address's low word alone */
    LM_OTHER_ADDRESS,   /* the leader adds at another offset than each lane's own */
    LM_STORE,           /* the leader's run stores */
    LM_LEAK,            /* what the run wrote is read after it */
    LM_ADDRESS_WRITTEN, /* the address changes before the leader's add */
    LM_PREDICATE_READ,  /* MATCH's predicate guards something else too */
    LM_COMPARED_OTHER,  /* MATCH compares what is no atomic's address */
    LM_NO_LEAVE,        /* each lane's own, then the leader's too */
    LM_NO_OWN,          /* no lane's own: the lanes leave with no add */
    LM_RESULT,          /* the run's last value of the result is no old value and rank */
    LM_CHANGES
};
static void ll_matched(Lone* p, u32 groups, int returns, int change) {
    ll_start(p);
    Word m = lop3_imm(20, 1, groups - 1, 255, 0xc0); /* LOP3 R20 = the thread & (groups - 1) */
    ll(p, m);
    Word x = alu(0x024, 4, 22, 20); /* IMAD R22 = R20 * 4 */
    set(&x, 32, 32, 4);
    set(&x, 64, 8, 255);
    ll(p, x);
    ll(p, iadd3(18, 2, 22));
    ll(p, mov_reg(19, 3));
    ll(p, mov_imm(9, change == LM_TWO ? 2 : 1));
    Word match = ll_match(change == LM_COMPARED_OTHER ? 2 : 18, 0);
    if (change == LM_ANY) set(&match, 79, 1, 1);
    if (change == LM_SINGLE) set(&match, 73, 1, 0);
    ll(p, match);
    if (change != LM_NO_OWN) ll(p, ll_if(returns ? atomg_add(9, 18, 0, 9) : ll_red(18, 9), 0, 1));
    u32 leave = 0;
    if (returns)
        leave = ll(p, bra(p->n, p->n, 0, 1)); /* patched */
    else if (change != LM_NO_LEAVE)
        ll(p, ll_if(exit_insn(), 0, 1));
    ll(p, ll_voteu(7));
    ll(p, ll_flo_ur(10, 7, 0));
    ll(p, ll_popc_ur(11, 7));
    if (change == LM_ADDRESS_WRITTEN) ll(p, iadd3_imm(18, 18, 4));
    Word eq = isetp(1, 7, 10, 14, 2, 0, 0, 0, 0, 7);
    ll(p, eq);
    const u32 imm = change == LM_OTHER_ADDRESS ? 16 : 0;
    Word lead = returns ? atomg_add(11, 18, imm, 11) : ll_red(18, 11);
    if (!returns) set(&lead, 40, 24, imm);
    ll(p, ll_if(lead, 1, 0));
    if (change == LM_STORE) ll(p, stg_nou(2, 4 * 200, 1));
    if (change == LM_LEAK) ll(p, mov_imm(23, 7));
    if (change == LM_PREDICATE_READ) ll(p, ll_if(mov_imm(24, 9), 0, 0));
    if (returns) {
        ll(p, ll_s2r(12, 0x39));
        ll(p, ll_lop3_ur(12, 12, 7));
        ll(p, ll_popc(12, 12));
        ll(p, ll_shfl(9, 11, 10));
        ll(p, iadd3(9, 9, 12));
        if (change == LM_RESULT) ll(p, mov_imm(9, 5));
        put(p->code, leave, bra(leave, p->n, 0, 1));
        /* the place: 32 a group, its threads by the value of R9 */
        Word g = alu(0x024, 4, 13, 20);
        set(&g, 32, 32, 32);
        set(&g, 64, 8, 255);
        ll(p, g);
        ll(p, iadd3(13, 13, 9));
        Word q = alu(0x024, 4, 13, 13);
        set(&q, 32, 32, 4);
        set(&q, 64, 8, 255);
        ll(p, q);
        ll(p, iadd3(10, 2, 13));
        ll(p, mov_reg(11, 3));
        ll(p, stg_nou(10, 4 * 8, 1));
        if (change == LM_LEAK) ll(p, stg_nou(2, 4 * 202, 23));
    }
    ll(p, exit_insn());
    ll(p, bra(p->n, p->n, 7, 0));
}

/* module with and without a warp, and the reason of a refusal */
static u32* ll_module(const Lone* p, int lanes, size_t* count, ShaderInfo* si) {
    *si = (ShaderInfo){0};
    si->cs_local[0] = 32, si->cs_local[1] = si->cs_local[2] = 1;
    return sm86_to_spirv_ftz(p->code, p->n * 16, ST_CS, NULL, si, count, SH_FTZ_EMULATE, lanes);
}
/* runs the program as a warp of 32 lanes and as 32 lone ones, from memory that is 0: NULL, or why not; the buffers they
   leave */
static const char* ll_run(const Lone* p, u32 warp[LL_WORDS], u32 lone[LL_WORDS], ShaderInfo* lone_info) {
    static u32 bank[1024], log[16 * SH_WLOG_PART];
    static SpvEvalEnv env[64];
    size_t nw = 0, nl = 0;
    ShaderInfo warp_info;
    u32 *with = ll_module(p, SM86_LANES_WARP, &nw, &warp_info), *without = ll_module(p, 0, &nl, lone_info);
    const char* why = with ? NULL : "the warp's module";
    if (!why && !without) why = "the lone module";
    bank[LL_BANK / 4] = LL_AT;
    for (int kind = 0; kind < 2 && !why; kind++) {
        u32* memory = kind ? lone : warp;
        const ShaderInfo* si = kind ? lone_info : &warp_info;
        memset(memory, 0, LL_WORDS * sizeof *memory);
        memset(log, 0, sizeof log);
        memset(env, 0, sizeof env);
        for (u32 l = 0; l < 32; l++) {
            for (u32 b = 0; b < SPV_EVAL_BINDINGS; b++) env[l].ubo[b] = bank, env[l].ubo_words[b] = 1024;
            for (int k = 0; k < si->ngbuf; k++)
                env[l].ubo[SNX_VKB_SSBO + k] = memory, env[l].ubo_words[SNX_VKB_SSBO + k] = LL_WORDS;
            env[l].ubo[SNX_VKB_WPAGES] = log, env[l].ubo_words[SNX_VKB_WPAGES] = 16 * SH_WLOG_PART;
            env[l].builtin[27][0] = env[l].builtin[29][0] = env[l].builtin[41][0] = l;
        }
        if (!kind)
            why = spv_eval_subgroup(with, nw, env, 32);
        else
            for (u32 l = 0; l < 32 && !why; l++) why = spv_eval(without, nl, &env[l]);
    }
    free(with);
    free(without);
    return why;
}
/* whether a module asks for subgroup operations: a GroupNonUniform capability (61 to 68) */
static int ll_subgroups(const u32* spv, size_t count) {
    for (size_t at = 5; at < count && spv[at] >> 16; at += spv[at] >> 16)
        if ((spv[at] & 0xffff) == 17 && spv[at + 1] >= 61 && spv[at + 1] <= 68) return 1;
    return 0;
}
static void ll_no_subgroups(const char* what, const Lone* p) {
    ShaderInfo info;
    size_t count = 0;
    u32* spv = ll_module(p, 0, &count, &info);
    CHECK(spv && !ll_subgroups(spv, count));
    if (!spv || ll_subgroups(spv, count))
        fprintf(stderr, "lone lanes, %s: %s\n", what,
                spv ? "the module has subgroup operations" : info.first_unsupported);
    free(spv);
    spv = ll_module(p, SM86_LANES_WARP, &count, &info);
    CHECK(spv && ll_subgroups(spv, count));
    free(spv);
}
/* a program no lone lane gives what the console gives, refused with the reason */
static void ll_refused(const char* what, const Lone* p, const char* reason) {
    ShaderInfo info;
    size_t count = 0;
    u32* spv = ll_module(p, SM86_LANES_WARP, &count, &info);
    CHECK(spv);
    if (!spv) fprintf(stderr, "lone lanes, %s: the warp's module: %s\n", what, info.first_unsupported);
    free(spv);
    spv = ll_module(p, 0, &count, &info);
    const int good = !spv && info.unsupported &&
                     !strncmp(info.first_unsupported, "subgroup ballot/shuffle unavailable: ", 37) &&
                     strstr(info.first_unsupported, reason);
    CHECK(good);
    if (!good)
        fprintf(stderr, "lone lanes, %s: %s, not refused for \"%s\"\n", what,
                spv ? "translated" : info.first_unsupported, reason);
    free(spv);
}
/* a program that lone lanes give the same words as a warp */
static void ll_equal(const char* what, const Lone* p, u32 counters) {
    u32 warp[LL_WORDS], lone[LL_WORDS], sum = 0;
    ShaderInfo info;
    const char* why = ll_run(p, warp, lone, &info);
    CHECK(!why);
    if (why) {
        fprintf(stderr, "lone lanes, %s: %s: %s\n", what, why, info.first_unsupported);
        return;
    }
    for (u32 k = 0; k < counters; k++) sum += warp[k];
    CHECK(sum == 32 && !memcmp(warp, lone, sizeof warp));
    if (sum != 32 || memcmp(warp, lone, sizeof warp))
        fprintf(stderr, "lone lanes, %s: the warp counts %u, lone lanes leave %s words\n", what, sum,
                memcmp(warp, lone, sizeof warp) ? "other" : "the same");
    ll_no_subgroups(what, p);
}
/* a program that a lone lane gives what the console gives: each of the 32 lanes its own place, and the same words of a
   warp's */
static void ll_accepted(const char* what, const Lone* p) {
    u32 warp[LL_WORDS], lone[LL_WORDS];
    ShaderInfo info;
    const char* why = ll_run(p, warp, lone, &info);
    CHECK(!why);
    if (why) {
        fprintf(stderr, "lone lanes, %s: %s: %s\n", what, why, info.first_unsupported);
        return;
    }
    u32 seen[LL_WORDS] = {0}, wrong = 0;
    for (u32 k = 0; k < 32; k++) {
        u32 thread = warp[LL_PLACES + k];
        wrong += thread >= 32 || seen[thread]++;
    }
    CHECK(warp[LL_COUNTER] == 32 && !wrong);
    if (warp[LL_COUNTER] != 32 || wrong)
        fprintf(stderr, "lone lanes, %s: the warp counts %u, %u places wrong\n", what, warp[LL_COUNTER], wrong);
    CHECK(!memcmp(warp, lone, sizeof warp));
    if (memcmp(warp, lone, sizeof warp)) fprintf(stderr, "lone lanes, %s: lone lanes leave other words\n", what);
}

static void test_lone_lanes(const char* dir) {
    Lone p;
    char what[64];
    (void)dir;
    for (u32 form = 0; form < 4; form++) {
        p = (Lone){0};
        ll_combined(&p, form & 1, form >> 1 & 1, LL_AS_IT_IS);
        snprintf(what, sizeof what, "%s leader, the %s lane", form & 1 ? "a branch's" : "a guard's",
                 form & 2 ? "lowest" : "highest");
        ll_accepted(what, &p);
        ll_no_subgroups(what, &p);
    }
    for (u32 groups = 1; groups <= 4; groups *= 2)
        for (int returns = 0; returns < 2; returns++) {
            p = (Lone){0};
            ll_matched(&p, groups, returns, LM_AS_IT_IS);
            snprintf(what, sizeof what, "a matched address, %u a warp, %s", groups,
                     returns ? "its old value taken" : "the program's end");
            ll_equal(what, &p, groups);
        }
}
static void test_lone_lanes_refused(void) {
    static const struct {
        int branch, lowest, change;
        const char* reason;
    } cases[] = {
        {0, 0, LL_DATA, "not the leader's add of the"},
        {1, 0, LL_DATA, "not the leader's add of the"},
        {0, 0, LL_LANE, "no combined add's"},
        {0, 0, LL_VALUE, "no combined add's"},
        {0, 0, LL_RANK, "IADD3"},
        {0, 0, LL_ADDRESS, "its address is not the warp's"},
        {0, 0, LL_NEGATED, "under a lane's predicate"},
        {0, 0, LL_MASK, "a lane mask"},
        {0, 0, LL_SEGMENT, "no combined add's"},
        {0, 0, LL_UP, "no combined add's"},
        {0, 0, LL_REDUX, "REDUX"},
        {0, 0, LL_STORE_LANE, "SR_LANEID"},
        {0, 1, LL_NO_SH, "the ballot reversed"},
        {1, 0, LL_RUN_STORE, "only some lanes run it"},
        {1, 0, LL_RUN_VALUE, "the leader's own value"},
        {1, 0, LL_RUN_PRED, "subgroup ballot/shuffle unavailable"},
        {0, 0, LL_VOTE_PRED, "not the running lanes' ballot"},
        {0, 0, LL_OF_PRED, "not the running lanes' ballot"},
        {0, 0, LL_OF_NOT, "not the running lanes' ballot"},
        {0, 0, LL_TABLE, "LOP3"},
        {0, 0, LL_COMPARE, "ISETP"},
        {0, 0, LL_MIN, "not the leader's add of the"},
        {0, 0, LL_NO_REV, "UFLO"},
        {0, 0, LL_OTHER, "not the leader's add of the"},
    };
    char what[64];
    for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
        Lone p = {0};
        ll_combined(&p, cases[k].branch, cases[k].lowest, cases[k].change);
        snprintf(what, sizeof what, "change %d, %s leader", cases[k].change,
                 cases[k].branch ? "a branch's" : "a guard's");
        ll_refused(what, &p, cases[k].reason);
    }
    static const struct {
        int returns, change;
        const char* reason;
    } matched[] = {
        {0, LM_TWO, "unavailable"},
        {1, LM_TWO, "unavailable"},
        {0, LM_ANY, "MATCH"},
        {0, LM_SINGLE, "MATCH"},
        {0, LM_OTHER_ADDRESS, "its address is not the warp's"},
        {1, LM_OTHER_ADDRESS, "its address is not the warp's"},
        {0, LM_STORE, "more than a combined add"},
        {1, LM_STORE, "more than a combined add"},
        {1, LM_LEAK, "the lanes' own value"},
        {1, LM_ADDRESS_WRITTEN, "its address is not the warp's"},
        {1, LM_PREDICATE_READ, "the lanes' addresses' predicate"},
        {0, LM_COMPARED_OTHER, "not its own's"},
        {1, LM_COMPARED_OTHER, "not its own's"},
        {0, LM_NO_LEAVE, "not its own's"},
        {0, LM_NO_OWN, "the lanes' addresses"},
        {1, LM_NO_OWN, "the lanes' addresses"},
        {1, LM_RESULT, "the lanes' own value"},
    };
    for (size_t k = 0; k < sizeof matched / sizeof *matched; k++) {
        Lone p = {0};
        ll_matched(&p, 2, matched[k].returns, matched[k].change);
        snprintf(what, sizeof what, "matched, change %d, %s", matched[k].change,
                 matched[k].returns ? "taken" : "ended");
        ll_refused(what, &p, matched[k].reason);
    }
}

/* a program of the longest length the proof takes, and one instruction more, which the reason counts */
static void test_lone_length(void) {
    enum { MOST = 12280 };
    u8* code = calloc(MOST + 1, 16);
    CHECK(code != NULL);
    if (!code) return;
    for (u32 n = MOST; n <= MOST + 1; n++) {
        ShaderInfo si = {0};
        size_t count = 0;
        char reason[64];
        si.cs_local[0] = 32, si.cs_local[1] = si.cs_local[2] = 1;
        put(code, 0, ll_voteu(7));
        for (u32 i = 1; i + 1 < n; i++) put(code, i, mov_imm(0, i));
        put(code, n - 1, exit_insn());
        u32* spv = sm86_to_spirv_ftz(code, n * 16, ST_CS, NULL, &si, &count, SH_FTZ_EMULATE, 0);
        snprintf(reason, sizeof reason, "unavailable: %u instructions, at most %u", n, (u32)MOST);
        const int good = n == MOST ? spv && count : !spv && si.unsupported && strstr(si.first_unsupported, reason);
        CHECK(good);
        if (!good) fprintf(stderr, "lone lanes, %u instructions: %s\n", n, spv ? "translated" : si.first_unsupported);
        free(spv);
    }
    free(code);
}

/* the stages a lone lane runs in with no proof: a fragment or vertex program that votes, finds itself or reduces has no
   subgroup operation, and gives what the console gives a lane alone; a fragment's reads inside its quad keep its lanes
   where the subgroups hold quads and are refused where they do not; and what no stage does without a warp is refused */
static u32* ll_stage(const Lone* p, int stage, int lanes, size_t* count, ShaderInfo* si) {
    static u32 sph[32];
    memset(sph, 0, sizeof sph);
    sph[0] = stage == ST_FS ? sph0(2, 5) : sph0(1, 1);
    sph[18] = 0xf; /* color 0, R0 to R3 */
    *si = (ShaderInfo){0};
    si->cs_local[0] = 32, si->cs_local[1] = si->cs_local[2] = 1;
    return sm86_to_spirv_ftz(p->code, p->n * 16, stage, stage == ST_CS ? NULL : sph, si, count, SH_FTZ_EMULATE, lanes);
}
static void ll_stage_refused(const char* what, const Lone* p, int stage, int lanes, const char* reason) {
    ShaderInfo si;
    size_t count = 0;
    u32* spv = ll_stage(p, stage, lanes, &count, &si);
    const int good = !spv && si.unsupported && strstr(si.first_unsupported, reason);
    CHECK(good);
    if (!good)
        fprintf(stderr, "lone lanes, %s: %s, not refused for \"%s\"\n", what, spv ? "translated" : si.first_unsupported,
                reason);
    free(spv);
}
static void test_lone_stages(void) {
    ShaderInfo si;
    size_t count = 0;
    /* R0 = the ballot, R1 = the lane, R2 = EQMASK, R3 = a reduction of 7 */
    Lone p = {0};
    ll(&p, ll_voteu(7));
    ll(&p, ll_popc_ur(0, 7)); /* 1 for a lane alone */
    ll(&p, ll_s2r(1, 0));
    ll(&p, ll_s2r(2, 0x38));
    ll(&p, mov_imm(3, 7));
    Word redux = ll_raw(0x00000000030673c4ull, 0x000e220000014000ull); /* REDUX UR6, R3 */
    ll(&p, redux);
    Word mov = ll_raw(0x0000000600037c02ull, 0x000fe20008000f00ull); /* MOV R3, UR6 */
    ll(&p, mov);
    ll(&p, exit_insn());
    ll(&p, bra(p.n, p.n, 7, 0));
    u32* spv = ll_stage(&p, ST_FS, 0, &count, &si);
    CHECK(spv && !ll_subgroups(spv, count));
    if (!spv) fprintf(stderr, "lone lanes, a fragment that votes: %s\n", si.first_unsupported);
    if (spv) {
        SpvEvalEnv env = {0};
        const char* why = eval_draw(spv, count, &env);
        const int good = !why && env.out[0][0] == 1 && env.out[0][1] == 0 && env.out[0][2] == 1 && env.out[0][3] == 7;
        CHECK(good);
        if (!good)
            fprintf(stderr, "lone lanes, a fragment's own: %s, count %u, lane %u, EQMASK %u, reduction %u\n",
                    why ? why : "ran", env.out[0][0], env.out[0][1], env.out[0][2], env.out[0][3]);
    }
    free(spv);
    spv = ll_stage(&p, ST_FS, SM86_LANES_WARP, &count, &si);
    CHECK(spv && ll_subgroups(spv, count));
    free(spv);
    spv = ll_stage(&p, ST_VS, 0, &count, &si);
    CHECK(spv && !ll_subgroups(spv, count));
    if (!spv) fprintf(stderr, "lone lanes, a vertex program that votes: %s\n", si.first_unsupported);
    free(spv);
    /* SHFL.BFLY in a quad (segments of 4), which a fragment's derivatives are made of */
    Lone q = {0};
    Word quad = fix(0xf89); /* SHFL.BFLY PT, R1, R0, 0x1, 0x1c03 */
    set(&quad, 16, 8, 1);
    set(&quad, 40, 13, 0x1c03);
    set(&quad, 53, 5, 1);
    set(&quad, 58, 2, 3);
    set(&quad, 81, 3, 7);
    ll(&q, quad);
    ll(&q, exit_insn());
    ll(&q, bra(q.n, q.n, 7, 0));
    spv = ll_stage(&q, ST_FS, SM86_LANES_QUAD, &count, &si);
    CHECK(spv && ll_subgroups(spv, count));
    free(spv);
    ll_stage_refused("a quad's read with no quads", &q, ST_FS, 0, "SHFL");
    ll_stage_refused("a compute program's quad", &q, ST_CS, SM86_LANES_QUAD, "SHFL");
    /* the same SHFL of the whole warp, in a fragment with quads: each keeps its own value */
    Lone w = {0};
    Word far = quad;
    set(&far, 40, 13, 0x1f);
    set(&far, 53, 5, 16);
    ll(&w, mov_imm(0, 9));
    ll(&w, far);
    ll(&w, exit_insn());
    ll(&w, bra(w.n, w.n, 7, 0));
    spv = ll_stage(&w, ST_FS, SM86_LANES_QUAD, &count, &si);
    CHECK(spv && !ll_subgroups(spv, count));
    if (spv) {
        SpvEvalEnv env = {0};
        const char* why = eval_draw(spv, count, &env);
        CHECK(!why && env.out[0][1] == 9);
    }
    free(spv);
    /* the tensor core's instructions, and the lane alone, read where no proof takes them */
    Lone m = {0};
    Word movm = fix(0x23a); /* MOVM.16.MT88 R0, R2 */
    set(&movm, 16, 8, 0);
    set(&movm, 24, 8, 2);
    ll(&m, movm);
    ll(&m, exit_insn());
    ll(&m, bra(m.n, m.n, 7, 0));
    ll_stage_refused("a tensor core's instruction", &m, ST_CS, 0, "MOVM at 0");
    Lone l = {0};
    ll_start(&l);
    ll(&l, stg_nou(2, 0, 14));
    ll(&l, exit_insn());
    ll(&l, bra(l.n, l.n, 7, 0));
    ll_stage_refused("the lane with no ballot", &l, ST_CS, 0, "SR_LANEID");
}
