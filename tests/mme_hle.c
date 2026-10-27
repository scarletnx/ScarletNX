/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* mme_hle.c's native macros against the interpreter: for each implementation a macro written here from what the
   implementation's comment says it does (none of a game's code), run by mme.c's interpreter and by the implementation
   over the same parameters and registers, must send the same methods with the same values and leave the same
   registers. The implementations are called directly (the macros here hash to something else). Where an
   implementation leaves a call to the interpreter, it must say so before sending anything */
#include "nx/gpu/maxwell/mme.c"
#include <stdarg.h>

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* ------------------------------------------------ what mme.c reaches outside itself */
#define SENT_MAX 0x10000
static u32 sent_m[SENT_MAX], sent_v[SENT_MAX], nsent;
void maxwell3d_method(GpuChannel* ch, u32 m, u32 v, int last) {
    /* constant buffer data stays held for the method after it (mme_out): every other method is written at once */
    CHECK(last == !((m & 0xFFF) >= 0x8E4 && (m & 0xFFF) <= 0x8F3));
    CHECK(nsent < SENT_MAX);
    ch->r3d[m & 0xFFF] = v; /* what the 3D engine keeps, which a macro may read back */
    sent_m[nsent] = m;
    sent_v[nsent] = v;
    nsent++;
}
int g_gpu_split;
void maxwell3d_front(GpuChannel* ch, u32 m, u32 v, int last) { maxwell3d_method(ch, m, v, last); }
int g_verbose;
void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }
/* this test is about the native macros and the interpreter: mme_jit.c's compiled forms are tested in mme_jit.c */
int mme_jit_run(GpuChannel* ch, MmeRec* rec, u32 idx, u32 np) {
    (void)ch, (void)rec, (void)idx, (void)np;
    return 0;
}

/* ------------------------------------------------ an assembler for the macros below */
enum { FETCH, MOVE, MOVE_ADDR, FETCH_SEND, MOVE_SEND, FETCH_ADDR, MOVE_ADDR_SEND_FETCH };
enum { ALU_ADD = 0, ALU_SUB = 2, ALU_AND = 10 };
#define EXIT (1u << 7)
static u32 prog[512], at;
static int label[32], fix_at[64], fix_label[64], nfix;
static void emit(u32 w) { prog[at++] = w; }
static u32 imm18(i32 v) { return ((u32)v & 0x3FFFF) << 14; }
/* dst = a + imm, with result operation rop */
static void addi(u32 rop, u32 dst, u32 a, i32 imm) { emit(1 | rop << 4 | dst << 8 | a << 11 | imm18(imm)); }
/* the method address: method m, increment incr */
static void maddr(u32 m, u32 incr) { addi(MOVE_ADDR, 0, 0, (i32)(m | incr << 12)); }
static void send_reg(u32 r) { addi(MOVE_SEND, 0, r, 0); }
static void send_imm(i32 v) { addi(MOVE_SEND, 0, 0, v); }
static void fetch(u32 dst) { addi(FETCH, dst, 0, 0); }
static void op_alu(u32 rop, u32 op, u32 dst, u32 a, u32 b) { emit(rop << 4 | dst << 8 | a << 11 | b << 14 | op << 17); }
/* dst = a with size bits of b from bit sb placed at bit db */
static void ins(u32 rop, u32 dst, u32 a, u32 b, u32 sb, u32 sz, u32 db) {
    emit(2 | rop << 4 | dst << 8 | a << 11 | b << 14 | sb << 17 | sz << 22 | db << 27);
}
static void readr(u32 dst, u32 a, i32 imm) { emit(5 | dst << 8 | a << 11 | imm18(imm)); }
/* an annulled branch (no delay slot) to label l when register a is zero (nz 0) or not (nz 1) */
static void br(u32 nz, u32 a, int l) {
    fix_at[nfix] = (int)at;
    fix_label[nfix++] = l;
    emit(7 | nz << 4 | 1u << 5 | a << 11);
}
static void here(int l) { label[l] = (int)at; }
static void end_macro(void) {
    int i;
    emit(1 | MOVE << 4 | EXIT); /* exit, then its delay slot */
    emit(1 | MOVE << 4);
    for (i = 0; i < nfix; i++) prog[fix_at[i]] |= imm18(label[fix_label[i]] - fix_at[i]);
}
static void begin_macro(void) {
    at = 0;
    nfix = 0;
}

/* ------------------------------------------------ running both */
static GpuChannel chan;
static u32 r3d_before[0x1000];
static u32 nat_m[SENT_MAX], nat_v[SENT_MAX], nat_n, nat_r3d[0x1000];
/* the macro assembled last, as slot 0 */
static void load(void) {
    u32 i;
    mme_upload_ptr(&chan, 0);
    for (i = 0; i < at; i++) mme_upload(&chan, prog[i]);
    chan.mme_start_ptr = 0;
    mme_bind(&chan, 0);
}
/* implementation hash over the np parameters p, then the interpreter over them, from the same registers: 1 when both
   ran and agreed, 0 when the implementation left the call to the interpreter (having sent nothing) */
static int compare(u64 hash, const u32* p, u32 np) {
    int k = mme_native_find(hash);
    u32 i;
    CHECK(k >= 0);
    memcpy(chan.mme_params, p, np * 4);
    memcpy(r3d_before, chan.r3d, sizeof r3d_before);
    nsent = 0;
    if (!mme_native_run(k, &chan, NULL, chan.mme_params, np)) {
        CHECK(nsent == 0);
        return 0;
    }
    nat_n = nsent;
    memcpy(nat_m, sent_m, nsent * 4);
    memcpy(nat_v, sent_v, nsent * 4);
    memcpy(nat_r3d, chan.r3d, sizeof nat_r3d);
    memcpy(chan.r3d, r3d_before, sizeof r3d_before);
    nsent = 0;
    mme_interpret(&chan, NULL, 0, np);
    CHECK(!chan.mme.running);
    for (i = 0; i < nsent && i < nat_n; i++)
        if (sent_m[i] != nat_m[i] || sent_v[i] != nat_v[i]) {
            fprintf(stderr, "hash %016llx, send %u: native %x = %x, interpreter %x = %x\n", (unsigned long long)hash, i,
                    nat_m[i], nat_v[i], sent_m[i], sent_v[i]);
            break;
        }
    CHECK(i == nsent && nsent == nat_n);
    CHECK(!memcmp(nat_r3d, chan.r3d, sizeof nat_r3d));
    return 1;
}
static u32 rng = 12345;
static u32 rnd(void) {
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}

/* ------------------------------------------------ the implementations */
/* constant buffer clear: select the buffer of size bytes at p0:p1 at offset 0, then p2 times four zero words */
static void cb_clear_macro(i32 size) {
    begin_macro();
    maddr(0x8E0, 1);
    send_imm(size);
    send_reg(1);
    fetch(2);
    send_reg(2);
    fetch(3);
    send_imm(0);
    maddr(0x8E4, 0);
    here(0);
    br(0, 3, 1);
    send_imm(0);
    send_imm(0);
    send_imm(0);
    send_imm(0);
    addi(MOVE, 3, 3, -1);
    br(0, 0, 0);
    here(1);
    end_macro();
    load();
}
static void test_cb_clear(void) {
    static const u32 sizes[2] = {0x5F00, 0x7000};
    static const u64 hashes[2] = {0x6C97861D891EDF7Eull, 0xD246FDDF3A6173D7ull};
    int s;
    for (s = 0; s < 2; s++) {
        u32 p[3] = {0x12, 0x34567800, 0}, n;
        cb_clear_macro((i32)sizes[s]);
        for (n = 0; n < 0x700; n += 0x95) {
            p[2] = n;
            CHECK(compare(hashes[s], p, 3));
        }
        CHECK(compare(hashes[s], p, 2)); /* p2 missing: read as 0 */
        p[2] = MME_LOOP_MAX + 1; /* a loop the interpreter's limit could cut */
        CHECK(!compare(hashes[s], p, 3));
    }
}
/* memory clear: an inline-to-memory line of p2 bytes at p0:p1, launched, then p2 / 4 zero words */
static void test_clear_memory(void) {
    u32 p[3] = {0x1, 0x2000, 0}, n;
    begin_macro();
    fetch(2);
    fetch(3);
    maddr(0x060, 1);
    send_reg(3);
    send_imm(1);
    send_reg(1);
    send_reg(2);
    maddr(0x06C, 1);
    send_imm(0x1011);
    maddr(0x06D, 0);
    here(0);
    br(0, 3, 1);
    send_imm(0);
    send_imm(0);
    send_imm(0);
    send_imm(0);
    addi(MOVE, 3, 3, -16);
    br(0, 0, 0);
    here(1);
    end_macro();
    load();
    for (n = 0; n <= 0x4000; n += 0x330) {
        p[2] = n;
        CHECK(compare(0xEE4D0004BEC8ECF4ull, p, 3));
    }
    p[2] = 20; /* not a multiple of 16: the code never reaches zero */
    CHECK(!compare(0xEE4D0004BEC8ECF4ull, p, 3));
    p[2] = (MME_LOOP_MAX + 1) * 16;
    CHECK(!compare(0xEE4D0004BEC8ECF4ull, p, 3));
}
/* raster bounding box: the low byte of 0x452 AND scratch 0xD34, at bit 4 of 0x0BB; the parameter unused */
static void test_raster_bounding_box(void) {
    u32 p[1], i;
    begin_macro();
    readr(2, 0, 0x452);
    readr(3, 0, 0xD34);
    op_alu(MOVE, ALU_AND, 2, 2, 3);
    maddr(0x0BB, 0);
    ins(MOVE_SEND, 0, 0, 2, 0, 8, 4);
    end_macro();
    load();
    for (i = 0; i < 64; i++) {
        p[0] = rnd();
        chan.r3d[0x452] = i & 1 ? rnd() : 0;
        chan.r3d[0xD34] = rnd();
        CHECK(compare(0xDB1341DBEB4C8AF7ull, p, 1));
    }
}
/* indexed draws from records with a count (mme_hle.c's multi_draw_indexed_count), written with the scratch registers
   read back where the implementation keeps them */
static void multi_draw_macro(void) {
    begin_macro();
    fetch(2); /* most */
    fetch(3); /* topology */
    fetch(4); /* padding */
    fetch(5); /* count */
    maddr(0xD08, 0);
    send_reg(3);
    maddr(0xD0A, 0);
    send_reg(5);
    readr(6, 0, 0xD1B);
    br(0, 6, 9);
    op_alu(MOVE, ALU_SUB, 6, 2, 5); /* the smaller of most and count, by the sign of their difference */
    ins(MOVE, 6, 0, 6, 31, 1, 0);
    br(0, 6, 1);
    maddr(0xD0A, 0);
    send_reg(2);
    addi(MOVE, 5, 2, 0);
    here(1);
    op_alu(MOVE, ALU_SUB, 6, 1, 5); /* the first below the end, by the sign */
    ins(MOVE, 6, 0, 6, 31, 1, 0);
    br(0, 6, 9);
    addi(MOVE, 7, 0, 1);
    ins(MOVE, 6, 3, 7, 0, 2, 26);
    maddr(0xD09, 0);
    send_reg(6);
    here(2); /* a draw */
    readr(7, 0, 0xD0A);
    op_alu(MOVE, ALU_SUB, 7, 1, 7);
    br(0, 7, 9);
    fetch(2); /* index count */
    fetch(3); /* instances */
    addi(MOVE_ADDR_SEND_FETCH, 0, 0, 0x5F7);
    fetch(5); /* base vertex */
    fetch(6); /* base instance */
    maddr(0x446, 0);
    send_reg(5);
    maddr(0x50D, 1);
    send_reg(5);
    send_reg(6);
    maddr(0x8E3, 1);
    send_imm(0x640);
    send_reg(5);
    send_reg(6);
    send_reg(1);
    br(0, 3, 4);
    readr(7, 0, 0xD08);
    here(3); /* an instance */
    maddr(0x586, 0);
    send_reg(7);
    maddr(0x5F8, 0);
    send_reg(2);
    maddr(0x585, 0);
    send_imm(0);
    addi(MOVE, 3, 3, -1);
    readr(7, 0, 0xD09);
    br(1, 3, 3);
    here(4); /* the padding, but after the last draw */
    addi(MOVE, 1, 1, 1);
    readr(7, 0, 0xD0A);
    op_alu(MOVE, ALU_SUB, 7, 1, 7);
    br(0, 7, 2);
    addi(MOVE, 7, 4, 0);
    here(5);
    br(0, 7, 2);
    fetch(0);
    addi(MOVE, 7, 7, -1);
    br(0, 0, 5);
    here(9); /* the registers and constants zeroed */
    maddr(0x446, 0);
    send_imm(0);
    maddr(0x50D, 1);
    send_imm(0);
    send_imm(0);
    maddr(0x8E3, 1);
    send_imm(0x640);
    send_imm(0);
    send_imm(0);
    send_imm(0);
    end_macro();
    load();
}
static void test_multi_draw(void) {
    u32 p[256], i, j, n, pad, draws = 0;
    multi_draw_macro();
    for (i = 0; i < 400; i++) {
        u32 first = rnd() % 4, most = rnd() % 8, count = rnd() % 8;
        pad = rnd() % 3;
        if (i % 7 == 0) count = 0x80000000u + rnd() % 4; /* the signed comparisons */
        if (most < first) most = first; /* fewer is the interpreter's: the records left would be counted to 2^32 */
        p[0] = first;
        p[1] = most;
        p[2] = rnd() & 0x0C00FFFF;
        p[3] = pad;
        p[4] = count;
        n = 5 + rnd() % 60;
        for (j = 5; j < n; j++) p[j] = (j - 5) % (5 + pad) == 1 ? rnd() % 4 : rnd();
        chan.r3d[0xD1B] = i % 5 ? 0xFFFFFFFF : 0;
        chan.r3d[0xD08] = chan.r3d[0xD09] = chan.r3d[0xD0A] = rnd();
        CHECK(compare(0x3F5E74B9C9A50164ull, p, n));
        draws += nsent > 14;
    }
    CHECK(draws > 100);
    /* records left past the draws counted to near 2^32, and a draw with as many instances: the interpreter's */
    chan.r3d[0xD1B] = 1;
    p[0] = 3, p[1] = 1, p[2] = 4, p[3] = 0, p[4] = 2;
    CHECK(!compare(0x3F5E74B9C9A50164ull, p, 5));
    p[0] = 0, p[1] = 1, p[4] = 1, p[5] = 3, p[6] = 0xFFFFFFFF;
    CHECK(!compare(0x3F5E74B9C9A50164ull, p, 10));
}
static void test_draws(void) {
    u32 p[3] = {0x0400000B, 0x14, 0x1234};
    /* byte count draw: BEGIN p0, the stride p1 to 0x4C6 and the byte count p2 to 0x48F, END */
    begin_macro();
    maddr(0x586, 0);
    send_reg(1);
    addi(MOVE_ADDR_SEND_FETCH, 0, 0, 0x4C6);
    addi(MOVE_ADDR_SEND_FETCH, 0, 0, 0x48F);
    maddr(0x585, 0);
    send_imm(0);
    end_macro();
    load();
    CHECK(compare(0xB5F74EDB717278ECull, p, 3));
    CHECK(compare(0xB5F74EDB717278ECull, p, 1));
    /* simple draw: BEGIN p0, the first vertex p1 to 0x35D and the count p2 to 0x35E, END */
    begin_macro();
    maddr(0x586, 0);
    send_reg(1);
    addi(MOVE_ADDR_SEND_FETCH, 0, 0, 0x35D);
    addi(MOVE_ADDR_SEND_FETCH, 0, 0, 0x35E);
    maddr(0x585, 0);
    send_imm(0);
    end_macro();
    load();
    CHECK(compare(0xDD6A7FA92A7D2674ull, p, 3));
}
/* constant buffer select at offset p0, the 0x5F00-byte buffer at scratch 0xD18 */
static void test_cb_select(void) {
    u32 p[1], i;
    begin_macro();
    readr(2, 0, 0xD18);
    maddr(0x8E0, 1);
    send_imm(0x5F00);
    ins(MOVE_SEND, 0, 0, 2, 24, 8, 0);
    ins(MOVE_SEND, 0, 0, 2, 0, 24, 8);
    ins(MOVE_SEND, 0, 0, 1, 0, 30, 2);
    end_macro();
    load();
    for (i = 0; i < 32; i++) {
        p[0] = rnd() ^ rnd() << 16;
        chan.r3d[0xD18] = rnd() ^ rnd() << 16;
        CHECK(compare(0xDBCD08D629ABC472ull, p, 1));
    }
}
/* stencil: 0x4E0 on, the nibbles of p0 to the front's operations and function and the back's */
static void test_stencil(void) {
    static const u8 front[4] = {4, 12, 8, 0}, back[4] = {20, 28, 24, 16};
    u32 p[1], i;
    begin_macro();
    maddr(0x4E0, 1);
    send_imm(1);
    for (i = 0; i < 4; i++) ins(MOVE_SEND, 0, 0, 1, front[i], 4, 0);
    maddr(0x566, 1);
    for (i = 0; i < 4; i++) ins(MOVE_SEND, 0, 0, 1, back[i], 4, 0);
    end_macro();
    load();
    for (i = 0; i < 16; i++) {
        p[0] = rnd() ^ rnd() << 16;
        CHECK(compare(0x290BBC50F4AE637Full, p, 1));
    }
}
/* a word in scratch register s: when p0 differs, method m gets 25 (or p0 when from_p0) and s takes p0 */
static void scratch_macro(u32 s, u32 m, int from_p0) {
    begin_macro();
    readr(2, 0, (i32)s);
    op_alu(MOVE, ALU_SUB, 2, 2, 1);
    br(0, 2, 0);
    maddr(m, 0);
    if (from_p0)
        send_reg(1);
    else
        send_imm(25);
    maddr(s, 0);
    send_reg(1);
    here(0);
    end_macro();
    load();
}
static void test_scratch(void) {
    u32 p[1];
    scratch_macro(0xD29, 0x4B2, 0);
    chan.r3d[0xD29] = 7;
    p[0] = 7;
    CHECK(compare(0xAE29BDE4BD4C7ED1ull, p, 1) && nat_n == 0);
    p[0] = 8;
    CHECK(compare(0xAE29BDE4BD4C7ED1ull, p, 1) && nat_n == 2);
    scratch_macro(0xD36, 0x376, 1);
    chan.r3d[0xD36] = 0x55;
    p[0] = 0x55;
    CHECK(compare(0x81C197B87D4DF426ull, p, 1) && nat_n == 0);
    p[0] = 0x56;
    CHECK(compare(0x81C197B87D4DF426ull, p, 1) && nat_n == 2);
}

int main(void) {
    test_cb_clear();
    test_clear_memory();
    test_raster_bounding_box();
    test_multi_draw();
    test_draws();
    test_cb_select();
    test_stencil();
    test_scratch();
    printf("mme_hle: %u checks passed\n", checks);
    return 0;
}
