/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* mme_jit.c's compiled macros against the interpreter, which is the oracle: every operation and result operation, both
   bitfield forms, register reads, every ALU, both branch conditions with and without their delay slot, an exit's delay
   slot, a taken branch in a delay slot, parameters short and past the end, the same code at two addresses (one compiled
   function), the sends the engine sees, and seeded random programs. A program the compiler can't follow has no
   compiled form and the interpreter runs it, as before */
#include "nx/gpu/maxwell/mme.c"
#include "nx/gpu/maxwell/mme_jit.c"
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
static u32 sent_m[256], sent_v[256], nsent;
void maxwell3d_method(GpuChannel* ch, u32 m, u32 v, int last) {
    (void)ch;
    CHECK(last == 1);
    CHECK(nsent < sizeof sent_m / sizeof sent_m[0]);
    sent_m[nsent] = m;
    sent_v[nsent] = v;
    nsent++;
}
int g_gpu_split;
void maxwell3d_front(GpuChannel* ch, u32 m, u32 v, int last) { maxwell3d_method(ch, m, v, last); }
int g_verbose;
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl, (void)fmt;
}
int mme_native_find(u64 hash) { return (void)hash, -1; }
const char* mme_native_name(int k) { return (void)k, "?"; }
u64 mme_native_hash(int k) { return (void)k, 0; }
int mme_native_run(int k, GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    (void)k, (void)ch, (void)rec, (void)p, (void)np;
    return 0;
}

/* ------------------------------------------------ instructions */
static u32 enc_imm(u32 op, u32 dst, u32 a, i32 imm, u32 rop, u32 exit) {
    return op | rop << 4 | exit << 7 | dst << 8 | a << 11 | ((u32)imm & 0x3FFFF) << 14;
}
static u32 enc_alu(u32 alu, u32 dst, u32 a, u32 b, u32 rop, u32 exit) {
    return alu << 17 | rop << 4 | exit << 7 | dst << 8 | a << 11 | b << 14;
}
static u32 enc_bf(u32 op, u32 dst, u32 a, u32 b, u32 rop, u32 src_bit, u32 mask, u32 dst_bit, u32 exit) {
    return op | rop << 4 | exit << 7 | dst << 8 | a << 11 | b << 14 | (src_bit & 31) << 17 | (mask & 31) << 22 |
           (dst_bit & 31) << 27;
}
static u32 enc_branch(u32 a, i32 imm, u32 nonzero, u32 annul, u32 exit) {
    return 7 | nonzero << 4 | annul << 5 | exit << 7 | a << 11 | ((u32)imm & 0x3FFFF) << 14;
}
#define NOP enc_imm(1, 0, 0, 0, 1, 0)

/* ------------------------------------------------ the channel and one comparison */
static GpuChannel chan;
static u32 next_word = 0x200;

/* uploads code as an upload batch of its own and binds it to slot, as NVN does */
static u32 upload(GpuChannel* ch, u32 slot, const u32* code, u32 n) {
    u32 at, i;
    if (next_word + n > 0x2000) next_word = 0x40;
    at = next_word;
    next_word += n + 1; /* the word after the batch is not this batch's: the hash stops there */
    mme_upload_ptr(ch, at);
    for (i = 0; i < n; i++) mme_upload(ch, code[i]);
    ch->mme_start_ptr = slot;
    mme_bind(ch, at);
    return at;
}

static int same_state(const GpuMme* a, const GpuMme* b) {
    return !memcmp(a->regs, b->regs, sizeof a->regs) && a->carry == b->carry && a->method == b->method &&
           a->incr == b->incr && a->np == b->np && a->next == b->next && a->steps == b->steps &&
           a->running == b->running && a->branch == b->branch && a->slot == b->slot && a->exiting == b->exiting &&
           (a->pc & 0x1FFF) == (b->pc & 0x1FFF) &&
           (!a->branch || (a->target & 0x1FFF) == (b->target & 0x1FFF));
}

/* runs slot through the compiled form and then the interpreter, both recording, and compares every send and the
   state they end in. 1 when there was a compiled form */
static int compare(GpuChannel* ch, u32 slot, u32 np, const u32* params, const char* what) {
    static MmeRec compiled, interpreted;
    GpuMme after;
    u32 i;
    memset(&compiled, 0, sizeof compiled);
    memset(&interpreted, 0, sizeof interpreted);
    for (i = 0; i < np; i++) ch->mme_params[i] = params[i];
    CHECK(mme_choose(ch, slot) == -1);
    if (!mme_jit_run(ch, &compiled, slot, np)) return 0;
    after = ch->mme;
    mme_interpret(ch, &interpreted, slot, np);
    if (compiled.n != interpreted.n || compiled.total != interpreted.total || compiled.hash != interpreted.hash ||
        compiled.read_after_send != interpreted.read_after_send) {
        fprintf(stderr, "%s: %u against %u sends, hashes %016llx and %016llx\n", what, compiled.total,
                interpreted.total, (unsigned long long)compiled.hash, (unsigned long long)interpreted.hash);
        CHECK(0);
    }
    for (i = 0; i < compiled.n; i++)
        if (compiled.m[i] != interpreted.m[i] || compiled.v[i] != interpreted.v[i]) {
            fprintf(stderr, "%s: send %u is %x = %x, the interpreter's %x = %x\n", what, i, compiled.m[i],
                    compiled.v[i], interpreted.m[i], interpreted.v[i]);
            CHECK(0);
        }
    if (!same_state(&after, &ch->mme)) {
        fprintf(stderr,
                "%s: the state after it is pc %x (interpreter %x), %u steps (interpreter %u), running %d (%d), "
                "branch %d (%d), slot %d (%d), exiting %d (%d), target %x (%x), regs %x %x %x %x %x %x %x (%x %x %x %x "
                "%x %x %x)\n",
                what, after.pc & 0x1FFF, ch->mme.pc & 0x1FFF, after.steps, ch->mme.steps, after.running,
                ch->mme.running, after.branch, ch->mme.branch, after.slot, ch->mme.slot, after.exiting,
                ch->mme.exiting, after.target & 0x1FFF, ch->mme.target & 0x1FFF, after.regs[1], after.regs[2],
                after.regs[3], after.regs[4], after.regs[5], after.regs[6], after.regs[7], ch->mme.regs[1],
                ch->mme.regs[2], ch->mme.regs[3], ch->mme.regs[4], ch->mme.regs[5], ch->mme.regs[6], ch->mme.regs[7]);
        CHECK(0);
    }
    return 1;
}

/* uploads and compares a program in slot 0 */
static int run(GpuChannel* ch, const u32* code, u32 n, u32 np, const u32* params, const char* what) {
    upload(ch, 0, code, n);
    return compare(ch, 0, np, params, what);
}

/* ------------------------------------------------ the handcrafted programs */
static void handcrafted(void) {
    static const u32 params[8] = {0x77, 0x11223344, 0xFFFF0000, 5, 0, 0xDEADBEEF, 0x0F0F0F0F, 0x80000000};
    u32 code[64], n;
    u32 i;

    /* a method and a send, then the exit's delay slot */
    n = 0;
    code[n++] = enc_imm(1, 0, 0, 0x1123, 2, 0); /* method 0x123, increment 1 */
    code[n++] = enc_alu(8, 0, 1, 0, 4, 0);      /* send r1 */
    code[n++] = enc_imm(1, 0, 0, 0, 1, 1);      /* exit */
    code[n++] = enc_imm(1, 0, 0, 0x888, 4, 0);  /* its delay slot: send 0x888 */
    code[n++] = enc_imm(1, 0, 0, 0x999, 4, 0);  /* never runs */
    CHECK(run(&chan, code, n, 1, params, "send and exit"));
    CHECK(nsent == 0); /* recording, not sending */

    /* every ALU, the four bitfield operations and register reads, all in one program */
    n = 0;
    code[n++] = enc_imm(1, 2, 0, 0x1234, 1, 0);
    code[n++] = enc_imm(1, 3, 0, 0x00F0, 1, 0);
    for (i = 0; i < 9; i++) {
        static const u32 alus[] = {0, 1, 2, 3, 8, 9, 10, 11, 12};
        code[n++] = enc_alu(alus[i], 4, 2, 3, 1, 0);
        code[n++] = enc_alu(8, 0, 4, 0, 4, 0);
    }
    for (i = 0; i < 8; i++) code[n++] = enc_alu(8, 5, 2, 3, i, 0);
    code[n++] = enc_imm(1, 6, 2, -5, 1, 0);
    code[n++] = enc_bf(2, 7, 2, 3, 1, 5, 9, 3, 0);
    code[n++] = enc_bf(3, 1, 2, 3, 1, 0, 7, 4, 0);
    code[n++] = enc_bf(4, 2, 2, 3, 1, 3, 0x1F, 0, 0);
    code[n++] = enc_imm(5, 3, 0, 0xD00, 0, 0); /* a register read: result() is not involved */
    code[n++] = enc_imm(1, 0, 3, 0, 4, 0);     /* send what it read */
    code[n++] = enc_imm(1, 0, 0, 0, 1, 1);
    code[n++] = NOP;
    CHECK(run(&chan, code, n, 8, params, "operations"));

    /* a taken branch's delay slot that is a branch: the fetch after it is the first branch's target */
    n = 0;
    code[n++] = enc_branch(0, 4, 0, 0, 0);      /* taken: target 4, delay slot 1 */
    code[n++] = enc_branch(0, 5, 0, 0, 0);      /* taken: target 6 */
    code[n++] = enc_imm(1, 0, 0, 0x222, 4, 0);  /* skipped */
    code[n++] = enc_imm(1, 0, 0, 0x333, 4, 0);  /* skipped */
    code[n++] = enc_imm(1, 0, 0, 0x444, 4, 0);  /* the second branch's delay slot, at the first's target */
    code[n++] = enc_imm(1, 0, 0, 0x555, 4, 0);  /* skipped */
    code[n++] = enc_imm(1, 0, 0, 0x666, 4, 0);
    code[n++] = enc_imm(1, 0, 0, 0, 1, 1);
    code[n++] = NOP;
    CHECK(run(&chan, code, n, 4, params, "branch in a delay slot"));

    /* an exit's delay slot that is a branch gets a delay slot of its own, and the call ends after it */
    n = 0;
    code[n++] = enc_imm(1, 0, 0, 0, 1, 1);     /* exit */
    code[n++] = enc_branch(0, 3, 0, 0, 0);     /* in the exit's delay slot: target 4 */
    code[n++] = enc_branch(0, 2, 0, 0, 0);     /* its delay slot, a branch: target 4, fetched at 4 */
    code[n++] = enc_imm(1, 0, 0, 0x333, 4, 0); /* skipped */
    code[n++] = enc_imm(1, 0, 0, 0x444, 4, 0); /* the delay slot, which ends the call */
    code[n++] = enc_imm(1, 0, 0, 0x555, 4, 0); /* skipped */
    CHECK(run(&chan, code, n, 4, params, "branch in an exit's delay slot"));

    /* both branch conditions with and without the delay slot, taken and not */
    for (i = 0; i < 8; i++) {
        u32 p[1];
        n = 0;
        code[n++] = enc_branch(1, 3, (i >> 1) & 1, (i >> 2) & 1, 0);
        code[n++] = enc_imm(1, 0, 0, 0x111, 4, 0); /* the delay slot, or the fallthrough */
        code[n++] = enc_imm(1, 0, 0, 0x222, 4, 0); /* the fallthrough only */
        code[n++] = enc_imm(1, 0, 0, 0x333, 4, 0); /* the target */
        code[n++] = enc_imm(1, 0, 0, 0, 1, 1);
        code[n++] = NOP;
        p[0] = i & 1; /* r1 = 1 takes the "not zero" condition, 0 the "zero" one */
        CHECK(run(&chan, code, n, 1, p, "branches"));
    }

    /* parameters short and past the end: a fetch reads 0 and the next one stays put */
    n = 0;
    code[n++] = enc_imm(1, 0, 0, 0x1123, 2, 0);
    code[n++] = enc_imm(1, 1, 0, 0, 0, 0); /* r1 = fetch_param() */
    code[n++] = enc_alu(8, 0, 1, 0, 4, 0); /* send it */
    code[n++] = enc_imm(1, 2, 0, 0, 3, 0); /* r2 = fetch_param(), send 0 */
    code[n++] = enc_alu(8, 0, 2, 0, 4, 0); /* send it */
    code[n++] = enc_imm(1, 0, 0, 0, 1, 1);
    code[n++] = NOP;
    CHECK(run(&chan, code, n, 0, params, "no parameters"));
    CHECK(run(&chan, code, n, 1, params, "one parameter"));
    CHECK(run(&chan, code, n, 2, params, "two parameters"));

    /* the same code at another address: the same compiled function, whose state follows the new start */
    {
        u32 same[5], other[5];
        n = 0;
        same[n] = enc_imm(1, 0, 0, 0x1123, 2, 0);
        other[n] = same[n];
        n++;
        same[n] = enc_alu(8, 0, 1, 0, 4, 0);
        other[n] = same[n];
        n++;
        same[n] = enc_imm(1, 0, 0, 0, 1, 1);
        other[n] = same[n];
        n++;
        same[n] = NOP;
        other[n] = same[n];
        n++;
        CHECK(n == 4);
        upload(&chan, 1, same, n);
        CHECK(compare(&chan, 1, 1, params, "first address"));
        upload(&chan, 2, other, n);
        CHECK(compare(&chan, 2, 1, params, "same code, another address"));
    }

    /* what the engine sees: the compiled form and the interpreter send the same methods, recording nothing */
    {
        static u32 r3d[0x1000], after_r3d[0x1000];
        GpuMme after;
        u32 p[1] = {0xCAFE};
        n = 0;
        code[n++] = enc_imm(1, 0, 0, 0xE20 | (1 << 12), 2, 0); /* a macro method: only its register takes it */
        code[n++] = enc_imm(1, 0, 0, 0x1234, 4, 0);
        code[n++] = enc_imm(1, 0, 0, 0x123 | (1 << 12), 2, 0);
        code[n++] = enc_alu(8, 0, 1, 0, 4, 0);
        code[n++] = enc_imm(1, 0, 0, 0, 1, 1);
        code[n++] = NOP;
        upload(&chan, 0, code, n);
        CHECK(mme_choose(&chan, 0) == -1);
        memcpy(r3d, chan.r3d, sizeof r3d);
        nsent = 0;
        for (i = 0; i < 1; i++) chan.mme_params[i] = p[i];
        CHECK(mme_jit_run(&chan, NULL, 0, 1));
        after = chan.mme;
        memcpy(after_r3d, chan.r3d, sizeof after_r3d);
        CHECK(nsent == 1 && sent_m[0] == 0x123 && sent_v[0] == 0xCAFE); /* 0xE20 only wrote its register */
        CHECK(after_r3d[0xE20] == 0x1234);
        memcpy(chan.r3d, r3d, sizeof r3d);
        nsent = 0;
        mme_interpret(&chan, NULL, 0, 1);
        CHECK(nsent == 1 && sent_m[0] == 0x123 && sent_v[0] == 0xCAFE);
        CHECK(!memcmp(after_r3d, chan.r3d, sizeof r3d));
        CHECK(same_state(&after, &chan.mme));
    }

    /* code the compiler can't follow has no compiled form: the interpreter runs it */
    n = 0;
    code[n++] = 6; /* the unimplemented operation */
    code[n++] = enc_imm(1, 0, 0, 0, 1, 1);
    code[n++] = NOP;
    CHECK(!run(&chan, code, n, 0, params, "operation 6"));
    n = 0;
    code[n++] = enc_alu(5, 0, 1, 0, 1, 0); /* an ALU operation mme.c doesn't know */
    code[n++] = enc_imm(1, 0, 0, 0, 1, 1);
    code[n++] = NOP;
    CHECK(!run(&chan, code, n, 0, params, "ALU operation 5"));
    n = 0;
    code[n++] = enc_branch(0, 10, 0, 0, 0); /* past the batch */
    code[n++] = enc_imm(1, 0, 0, 0, 1, 1);
    code[n++] = NOP;
    CHECK(!run(&chan, code, n, 0, params, "a branch past the batch"));
}

/* ------------------------------------------------ seeded random programs */
static u64 fuzz_state = 0x9E3779B97F4A7C15ull;
static u32 random32(void) {
    u64 x = fuzz_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    fuzz_state = x;
    return (u32)(x >> 32);
}

static u32 random_insn(void) {
    static const u32 ops[] = {0, 1, 2, 3, 4, 5};
    static const u32 alus[] = {0, 1, 2, 3, 8, 9, 10, 11, 12};
    u32 op = ops[random32() % (sizeof ops / sizeof ops[0])];
    u32 rop = random32() & 7, dst = random32() & 7, a = random32() & 7, b = random32() & 7;
    u32 exit = random32() & 1;
    switch (op) {
        case 0: return enc_alu(alus[random32() % (sizeof alus / sizeof alus[0])], dst, a, b, rop, exit);
        case 1: return enc_imm(1, dst, a, (i32)(random32() & 0x3FFFF) - 0x20000, rop, exit);
        case 2:
        case 3:
        case 4:
            return enc_bf(op, dst, a, b, rop, random32() & 31, random32() & 31, random32() & 31, exit);
        default: return enc_imm(5, dst, a, (i32)(random32() & 0xFFF), rop, exit);
    }
}

/* forward branches only, never in another branch's delay slot, and an exit at the end: every program ends */
static u32 random_program(u32* code, u32 most) {
    u32 n = 8 + random32() % (most - 8), i, delayed = 0;
    for (i = 0; i < n - 2; i++) {
        if (!delayed && i + 5 <= n - 2 && random32() % 6 == 0) {
            u32 target = i + 2 + random32() % (n - 3 - (i + 2) + 1);
            code[i] = enc_branch(random32() & 7, (i32)target - (i32)i, random32() & 1, random32() & 1, random32() & 1);
            delayed = 1;
        } else {
            code[i] = random_insn();
            delayed = 0;
        }
    }
    code[n - 2] = enc_imm(1, 0, 0, 0, 1, 1);
    code[n - 1] = NOP;
    return n;
}

static void fuzz(u32 count) {
    static u32 code[64], params[4];
    u32 i, j, np;
    for (i = 0; i < count; i++) {
        u32 n = random_program(code, 48);
        np = random32() % 5;
        for (j = 0; j < np; j++) params[j] = random32();
        upload(&chan, 0, code, n);
        CHECK(compare(&chan, 0, np, params, "random program"));
    }
}

int main(void) {
    u32 i;
    const char* count;
    for (i = 0; i < 0x1000; i++) chan.r3d[i] = i * 0x9E3779B9u;
    handcrafted();
    count = getenv("SNX_MME_FUZZ");
    fuzz(count ? (u32)strtoul(count, NULL, 10) : 1000);
    mme_jit_free();
    printf("mme_jit: %u checks passed\n", checks);
    return 0;
}
