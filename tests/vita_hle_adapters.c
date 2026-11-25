/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The adapters tools/hookgen --vita generates (tests/hook/vita.hook): each reads its function's arguments from the
   calling thread where AAPCS32 with VFP arguments puts them (r0-r3, an even pair of them for 64 bits, s and d
   registers apart, the rest on the stack; a variadic function's floats in core registers), hands them to its
   handler and returns the result as r0 (a 64-bit one's high word in r1, a float's in s0 or d0, r0 as it was for
   void). The table is sorted by NID and ends with a NULL-named entry */
#include "vita/hle/test.hook.h"
#include "vita/hle/test.hook.inc"
#include "common/utilities/common.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t seen[4];
static double seen_f[2];
static int calls;

int64_t test_mixed(VitaProcess* p, VitaThread* t, uint32_t self, int32_t a, float b, uint64_t w, double d,
                   uint32_t c2) {
    (void)p, (void)t;
    seen[0] = self, seen[1] = (uint64_t)(int64_t)a, seen[2] = w, seen[3] = c2;
    seen_f[0] = b, seen_f[1] = d;
    return -0x100000002ll;
}
int8_t test_narrow(VitaProcess* p, VitaThread* t, int8_t a) {
    (void)p, (void)t;
    return (int8_t)(a - 1);
}
bool test_flag(VitaProcess* p, VitaThread* t, bool on) {
    (void)p, (void)t;
    return !on;
}
double test_half(VitaProcess* p, VitaThread* t, double x) {
    (void)p, (void)t;
    return x / 2;
}
float test_single(VitaProcess* p, VitaThread* t, float x) {
    (void)p, (void)t;
    return x * 2;
}
uint32_t test_stacked(VitaProcess* p, VitaThread* t, uint32_t a, uint32_t b, uint32_t c, uint64_t d, uint32_t e) {
    (void)p, (void)t;
    seen[0] = a, seen[1] = b, seen[2] = c, seen[3] = d;
    return e;
}
void test_nothing(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    calls++;
}
int32_t test_print(VitaProcess* p, VitaThread* t, uint32_t format, double x) {
    seen[0] = format, seen_f[0] = x;
    return (int32_t)vita_arg(p, t, 4); /* the first variadic word, past the pair r2:r3 */
}

static const VitaHle* entry(uint32_t nid) {
    for (const VitaHle* h = vita_test_table; h->name; ++h)
        if (h->nid == nid) return h;
    return NULL;
}
static void set_s(VitaThread* t, unsigned n, float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    t->cpu.d[n / 2] = (t->cpu.d[n / 2] & ~(0xffffffffull << (n & 1 ? 32 : 0))) | (uint64_t)bits << (n & 1 ? 32 : 0);
}
static void set_d(VitaThread* t, unsigned n, double d) { memcpy(&t->cpu.d[n], &d, sizeof d); }

int main(void) {
    char *root = snx_temp_dir(NULL), *folder = root ? snx_path_join(root, "game") : NULL, error[256] = {0};
    assert(folder && !snx_mkdirs(folder));
    VitaImage image = {0};
    VitaProcess* p = vita_process_new(&image, folder, error, sizeof error);
    if (!p) fprintf(stderr, "vita_process_new: %s\n", error);
    assert(p);
    VitaThread* t = &p->threads[0];
    p->current = t;
    t->cpu.r[13] -= 0x100; /* room for the stack's arguments */
    uint32_t sp = t->cpu.r[13];

    /* sorted by NID, ended by a NULL-named entry; each with its spec's family */
    assert(VITA_TEST_COUNT == 9 && !vita_test_table[VITA_TEST_COUNT].name);
    for (int i = 1; i < VITA_TEST_COUNT; ++i) assert(vita_test_table[i - 1].nid < vita_test_table[i].nid);
    assert(entry(0xabcd) && !strcmp(entry(0xabcd)->name, "flag") && !strcmp(entry(0xabcd)->family, "types"));
    assert(entry(0x20) && !strcmp(entry(0x20)->family, "other"));

    /* r0, r1, s0, r2:r3, d1, the stack's first word; an i64 result in r0 and r1 */
    t->cpu.r[0] = 0x81000000u, t->cpu.r[1] = (uint32_t)-5, t->cpu.r[2] = 0x55667788u, t->cpu.r[3] = 0x11223344u;
    set_s(t, 0, 1.5f), set_s(t, 1, 99.0f), set_d(t, 1, -2.25);
    vita_write32(p, sp, 77);
    uint32_t r0 = entry(1)->function(p, t);
    assert(seen[0] == 0x81000000u && seen[1] == (uint64_t)-5 && seen[2] == 0x1122334455667788ull && seen[3] == 77);
    assert(seen_f[0] == 1.5 && seen_f[1] == -2.25);
    assert(r0 == 0xfffffffeu && t->cpu.r[1] == 0xfffffffeu);

    /* narrow results sign-extended to 32 bits; a bool its low byte */
    t->cpu.r[0] = 0x80u; /* -128 as an i8 */
    assert(entry(2)->function(p, t) == 0x7fu);
    t->cpu.r[0] = 0xffffff00u;
    assert(entry(0x30)->function(p, t) == 0xffffffffu);
    t->cpu.r[0] = 0x100u; /* false: its low byte is 0 */
    assert(entry(0xabcd)->function(p, t) == 1);

    /* d0 and s0, r0 kept */
    set_d(t, 0, 3.0);
    t->cpu.r[0] = 1234;
    assert(entry(0xfffffffeu)->function(p, t) == 1234 && vita_double(t, 0) == 1.5);
    set_s(t, 0, 2.5f);
    assert(entry(0x10)->function(p, t) == 1234 && vita_float(t, 0) == 5.0f);

    /* a 64-bit argument past r2 on the stack (r3 left), the next integer after it */
    t->cpu.r[0] = 1, t->cpu.r[1] = 2, t->cpu.r[2] = 3, t->cpu.r[3] = 0xdead;
    vita_write32(p, sp, 0x89abcdefu), vita_write32(p, sp + 4, 0x01234567u), vita_write32(p, sp + 8, 42);
    assert(entry(0x11)->function(p, t) == 42);
    assert(seen[0] == 1 && seen[1] == 2 && seen[2] == 3 && seen[3] == 0x0123456789abcdefull);

    /* void: r0 as it was */
    t->cpu.r[0] = 0x5555;
    assert(entry(0x20)->function(p, t) == 0x5555 && calls == 1);

    /* variadic: the double in r2:r3 as its bits, the rest read by the handler */
    double x = -6.5;
    uint64_t bits;
    memcpy(&bits, &x, sizeof bits);
    t->cpu.r[0] = 0x81000010u, t->cpu.r[1] = 0xdead, t->cpu.r[2] = (uint32_t)bits, t->cpu.r[3] = (uint32_t)(bits >> 32);
    vita_write32(p, sp, (uint32_t)-9);
    assert(entry(0x21)->function(p, t) == (uint32_t)-9 && seen[0] == 0x81000010u && seen_f[0] == -6.5);

    vita_process_free(p);
    assert(!snx_remove_tree(root));
    free(folder);
    free(root);
    puts("vita_hle_adapters: ok");
    return 0;
}
