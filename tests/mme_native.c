/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* mme.c's choice of a native macro by the hash of its code, with macros written here (none of a game's): the hash
   is Eden's (a vector computed apart from this code); a macro whose code hashes to a known implementation runs it
   and the interpreter doesn't, with the same methods sent; unknown code, a word more in the same upload batch, or
   new code over a chosen macro keep the interpreter, while a batch uploaded right after doesn't count; a native
   implementation can leave a call to the interpreter; a send to a macro method only sets its register; and the
   verify mode sends the interpreter's methods once, catches a native implementation that sends something else, and
   doesn't judge a macro that reads a register after sending */
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
static u32 sent_m[64], sent_v[64], nsent;
void maxwell3d_method(GpuChannel* ch, u32 m, u32 v, int last) {
    (void)ch;
    CHECK(last == 1);
    CHECK(nsent < 64);
    sent_m[nsent] = m;
    sent_v[nsent] = v;
    nsent++;
}
int g_gpu_split;
void maxwell3d_front(GpuChannel* ch, u32 m, u32 v, int last) { maxwell3d_method(ch, m, v, last); }
static char logged[4096];
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    size_t n = strlen(logged);
    (void)lvl;
    va_start(ap, fmt);
    if (n < sizeof logged) vsnprintf(logged + n, sizeof logged - n, fmt, ap);
    va_end(ap);
}

/* the native implementations: A's, which can be told to send a wrong value or to leave the call to the interpreter,
   and C's. The hashes were computed apart from mme.c, over the code words below */
#define HASH_A 0xD0A150BDE6341908ull
#define HASH_C 0x8FFC996343341809ull
static int a_runs, a_wrong, a_decline;
static int native_a(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    a_runs++;
    if (a_decline) return 0;
    mme_out(ch, rec, 0x123, np ? p[0] : 0);
    mme_out(ch, rec, 0x124, a_wrong ? 6 : 5);
    return 1;
}
static int native_c(GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    mme_out(ch, rec, 0x200, np ? p[0] : 0);
    (void)mme_reg(ch, rec, 0xD00);
    return 1;
}
int mme_native_find(u64 hash) { return hash == HASH_A ? 0 : hash == HASH_C ? 1 : -1; }
const char* mme_native_name(int k) { return k ? "C" : "A"; }
u64 mme_native_hash(int k) { return k ? HASH_C : HASH_A; }
int mme_native_run(int k, GpuChannel* ch, MmeRec* rec, const u32* p, u32 np) {
    return (k ? native_c : native_a)(ch, rec, p, np);
}
/* this test is about the native choice and the interpreter: mme_jit.c's compiled forms are elsewhere */
int mme_jit_run(GpuChannel* ch, MmeRec* rec, u32 idx, u32 np) {
    (void)ch, (void)rec, (void)idx, (void)np;
    return 0;
}

/* ------------------------------------------------ macros */
/* A: method 0x123 (incr 1), send r1 (p0), exit; its delay slot fetches and sends 5 (to 0x124) */
static const u32 code_a[] = {0x0448C021, 0x000008C1, 0x00014231};
/* B: A sending 6 */
static const u32 code_b[] = {0x0448C021, 0x000008C1, 0x00018231};
/* C: method 0x200, send r1, read 0xD00 and exit, a no-op slot */
static const u32 code_c[] = {0x00800021, 0x00000841, 0x03400295, 0x00000011};

static GpuChannel chan;
static void upload(GpuChannel* ch, u32 at, const u32* code, u32 n) {
    u32 i;
    mme_upload_ptr(ch, at);
    for (i = 0; i < n; i++) mme_upload(ch, code[i]);
}
static void bind(GpuChannel* ch, u32 slot, u32 start) {
    ch->mme_start_ptr = slot;
    mme_bind(ch, start);
}
/* calls macro slot with p0 = 0x77 */
static void call(GpuChannel* ch, u32 slot) {
    nsent = 0;
    logged[0] = 0;
    ch->mme_params[0] = 0x77;
    mme_run(ch, slot, 1);
}
static int sent_a(void) {
    return nsent == 2 && sent_m[0] == 0x123 && sent_v[0] == 0x77 && sent_m[1] == 0x124 && sent_v[1] == 5;
}

int main(void) {
    static const u32 vec[] = {0x00000011, 0x12345678, 0xFFFFFFFF, 0};
    GpuChannel* ch = &chan;
    u64 h = 0;
    u32 i, zero = 0;
    int runs;
    for (i = 0; i < 4; i++) h = mme_hash_word(h, vec[i]);
    CHECK(h == 0x1ACB25143B361588ull);

    mme_mode = MME_NATIVE;
    /* A in a batch of its own at 0x10, B right after it in another: each slot hashes its own batch */
    upload(ch, 0x10, code_a, 3);
    upload(ch, 0x13, code_b, 3);
    bind(ch, 3, 0x10);
    bind(ch, 4, 0x13);
    a_runs = 0;
    call(ch, 3);
    CHECK(a_runs == 1 && sent_a() && mme_n_native == 1);
    CHECK(strstr(logged, "hash d0a150bde6341908: A") != NULL);
    call(ch, 4); /* unknown: interpreted */
    CHECK(a_runs == 1 && mme_n_interp == 1 && nsent == 2 && sent_v[1] == 6);
    call(ch, 3); /* the choice is kept: no new lookup */
    CHECK(a_runs == 2 && sent_a() && logged[0] == 0);

    /* the native one leaves the call to the interpreter, which sends the same */
    a_decline = 1;
    call(ch, 3);
    CHECK(a_runs == 3 && sent_a() && mme_n_interp == 2);
    a_decline = 0;

    /* a word more in A's batch changes its hash; the interpreter runs A */
    upload(ch, 0x20, code_a, 3);
    mme_upload(ch, zero);
    bind(ch, 5, 0x20);
    runs = a_runs;
    call(ch, 5);
    CHECK(a_runs == runs && sent_a());

    /* B uploaded over A: slot 3 is looked up again, and interpreted */
    upload(ch, 0x10, code_b, 3);
    call(ch, 3);
    CHECK(a_runs == runs && nsent == 2 && sent_v[1] == 6);
    upload(ch, 0x10, code_a, 3);
    call(ch, 3);
    CHECK(a_runs == runs + 1 && sent_a());

    /* mme_hle=0's mode: everything interpreted */
    mme_mode = 0;
    call(ch, 3);
    CHECK(a_runs == runs + 1 && sent_a());

    /* a macro's send to a macro method sets its register and starts nothing: method 0xE20 from the native path */
    mme_out(ch, NULL, 0xE20, 0x1234);
    CHECK(ch->r3d[0xE20] == 0x1234);

    /* verify: both run, the interpreter's methods are sent once, a difference is caught */
    mme_mode = MME_NATIVE | MME_VERIFY;
    call(ch, 3);
    CHECK(sent_a() && mme_n_same == 1 && mme_n_differ == 0 && strstr(logged, "differs") == NULL);
    a_wrong = 1;
    call(ch, 3);
    CHECK(sent_a() && mme_n_differ == 1);
    CHECK(strstr(logged,
                 "mme: macro 3 (A, hash d0a150bde6341908) differs from the interpreter at its send 1: native 124 = 6, "
                 "interpreter 124 = 5") != NULL);
    a_wrong = 0;
    /* C reads a register after its send: the recording can't tell what the engine would have read */
    upload(ch, 0x30, code_c, 4);
    bind(ch, 6, 0x30);
    call(ch, 6);
    CHECK(mme_n_unjudged == 1 && mme_n_differ == 1 && nsent == 1 && sent_m[0] == 0x200 && sent_v[0] == 0x77);
    printf("mme_native: %u checks passed\n", checks);
    return 0;
}
