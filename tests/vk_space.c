/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The free space of a block of device memory (vk/space.c), against a map of the block's units: what a request gets
   lies in the block at the alignment asked for and on no unit another request holds, a request is refused only when
   no free place holds it, free neighbours join whatever their sizes, and a block given back whole is one run again */
#include "nx/gpu/vk/space.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);        \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static u32 g_seed = 0x2545f491u;
static u32 rnd(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

typedef struct {
    u32 at, n;
} Held;

static int check_exact(void) {
    Space s;
    u32 a, c, d, e;
    CHECK(!space_init(&s, 0));
    CHECK(space_init(&s, 16));
    CHECK(s.largest == 16 && !s.used && s.nrun == 1);
    CHECK(space_take(&s, 5, 1, &a) && a == 0 && s.used == 5);
    CHECK(space_take(&s, 2, 1, &c) && c == 5);
    CHECK(space_take(&s, 1, 1, &d) && d == 7);
    CHECK(s.largest == 8);
    CHECK(space_take(&s, 1, 4, &e) && e == 8); /* at its alignment */
    CHECK(!space_take(&s, 9, 1, &e) && !space_take(&s, 0, 1, &e) && !space_take(&s, 1, 3, &e));
    /* a part of a request given back: the next request of that size takes the hole, not the long run */
    space_give(&s, 3, 2);
    CHECK(s.used == 7 && s.nrun == 2);
    CHECK(space_take(&s, 2, 1, &e) && e == 3);
    space_give(&s, 3, 2);
    space_give(&s, 0, 3);
    CHECK(s.nrun == 2 && s.run[0].at == 0 && s.run[0].n == 5);
    space_give(&s, c, 2);
    space_give(&s, d, 1);
    space_give(&s, 8, 1); /* joins the runs on both sides */
    CHECK(!s.used && s.largest == 16 && s.nrun == 1);
    /* the whole block in one request, and unit by unit */
    CHECK(space_take(&s, 16, 1, &a) && a == 0 && !s.largest && !s.nrun);
    space_give(&s, 0, 16);
    for (u32 i = 0; i < 16; i++) CHECK(space_take(&s, 1, 1, &a) && a == i);
    CHECK(!space_take(&s, 1, 1, &a));
    /* two free neighbours that are not the halves of one piece join */
    space_give(&s, 1, 1);
    space_give(&s, 2, 1);
    CHECK(s.largest == 2 && s.nrun == 1);
    CHECK(space_take(&s, 2, 1, &a) && a == 1);
    for (u32 i = 0; i < 16; i += 2) space_give(&s, i, 1);
    CHECK(s.largest == 1 && s.used == 8 && s.nrun == 8);
    for (u32 i = 1; i < 16; i += 2) space_give(&s, i, 1);
    CHECK(s.largest == 16 && s.nrun == 1);
    /* an alignment inside a run leaves both ends of it free */
    CHECK(space_take(&s, 3, 1, &a) && a == 0);
    CHECK(space_take(&s, 4, 8, &c) && c == 8 && s.nrun == 2 && s.run[0].at == 3 && s.run[0].n == 5);
    space_give(&s, a, 3);
    space_give(&s, c, 4);
    CHECK(s.nrun == 1 && s.largest == 16);
    space_destroy(&s);
    CHECK(!s.run);
    return 0;
}

/* whether n free units lie at a multiple of align in the map */
static int fits(const u8* map, u32 units, u32 n, u32 align) {
    for (u32 a = 0; a + n <= units && a + n >= a; a += align) {
        u32 k = 0;
        while (k < n && !map[a + k]) k++;
        if (k == n) return 1;
    }
    return 0;
}

static int check_random(u32 units, u32 rounds, int exact) {
    Space s;
    u8* map = calloc(units, 1);
    u32 cap = 4096, n = 0;
    Held* held = calloc(cap, sizeof *held);
    CHECK(map && held && space_init(&s, units));
    u64 used = 0;
    for (u32 r = 0; r < rounds; r++) {
        int take = n < cap && (!n || rnd() % 5 < 3);
        if (take) {
            u32 big = units / 4 ? units / 4 : 1;
            u32 want = 1 + rnd() % (rnd() % 8 ? 64 : big), align = 1u << (rnd() % 6), at = 0;
            int got = space_take(&s, want, align, &at);
            if (!got) {
                if (exact) CHECK(!fits(map, units, want, align)); /* refused only when nothing holds it */
                continue;
            }
            CHECK(at % align == 0 && at + want <= units);
            for (u32 u = at; u < at + want; u++) {
                CHECK(!map[u]);
                map[u] = 1;
            }
            held[n++] = (Held){at, want};
            used += want;
        } else {
            u32 k = rnd() % n;
            Held h = held[k];
            /* in two parts, sometimes */
            u32 first = h.n > 1 && rnd() % 3 == 0 ? 1 + rnd() % (h.n - 1) : h.n;
            space_give(&s, h.at + (h.n - first), first);
            if (first < h.n) space_give(&s, h.at, h.n - first);
            memset(map + h.at, 0, h.n);
            held[k] = held[--n];
            used -= h.n;
        }
        CHECK(s.used == used);
        /* the runs are in order, apart, and the longest is known */
        if (r % 512 == 0) {
            u32 most = 0;
            for (u32 i = 0; i < s.nrun; i++) {
                CHECK(s.run[i].n && s.run[i].at + s.run[i].n <= units);
                if (i) CHECK(s.run[i - 1].at + s.run[i - 1].n < s.run[i].at);
                if (s.run[i].n > most) most = s.run[i].n;
            }
            CHECK(most == s.largest);
        }
    }
    while (n) {
        Held h = held[--n];
        space_give(&s, h.at, h.n);
    }
    CHECK(!s.used && s.largest == units && s.nrun == 1 && s.run[0].at == 0 && s.run[0].n == units);
    space_destroy(&s);
    free(map);
    free(held);
    return 0;
}

int main(void) {
    CHECK(!check_exact());
    CHECK(!check_random(1u << 10, 100000, 1));
    CHECK(!check_random(1u << 16, 200000, 0)); /* a 64 MiB block of 1 KiB units */
    CHECK(!check_random(1, 100, 1));
    CHECK(!check_random(1000, 20000, 1)); /* not a power of two */
    puts("PASS space: exact sizes, alignment, no overlap, free neighbours join");
    return 0;
}
