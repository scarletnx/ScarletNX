/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The shader lookups' tables (src/nx/gpu/vk/lookup_ways.h) hold a frame's keys: those a set has no room for are read
   and hashed again every time they come back. Keys from a fixed generator, as many as one open world's frame had
   (about 1,500 draw keys and some 64 compute programs) and more: the keys past their set's ways are counted. With two
   ways in 2048 sets, 1,500 draw keys left 88 past them; with one way in 64 sets, 64 programs left 23 */
#include "nx/gpu/vk/lookup_ways.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u64 g_state = 0x5EED5EED5EED5EEDull;
static u64 next(void) { /* splitmix64 */
    u64 z = (g_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static int failures;
/* keys placed by set (sets of them, ways each): those past the ways of their set */
static u32 past(const u32* set_of, u32 keys, u32 sets, u32 ways) {
    u32* n = calloc(sets, sizeof *n);
    u32 over = 0;
    for (u32 i = 0; i < keys; i++)
        if (++n[set_of[i]] > ways) over++;
    free(n);
    return over;
}
static void expect(const char* what, u32 got, u32 most) {
    printf("%s: %u keys past their set's ways (at most %u)\n", what, got, most);
    if (got > most) failures++;
}
int main(void) {
    static u32 set_of[8192];
    const u32 sets = 1u << VK_SHA_SET_BITS;
    for (u32 keys = 1500; keys <= 2000; keys += 500) {
        g_state = 0x5EED5EED5EED5EEDull;
        for (u32 i = 0; i < keys; i++) set_of[i] = vk_sha_set(next());
        char what[64];
        snprintf(what, sizeof what, "%u draw keys", keys);
        expect(what, past(set_of, keys, sets, VK_SHA_WAYS), 2);
    }
    /* programs 256-byte aligned within 16 MiB of code, as a game's compute programs lie */
    g_state = 0xC0DEC0DEC0DEC0DEull;
    for (u32 i = 0; i < 64; i++) set_of[i] = vk_csf_set(0x400000000ull + (next() & 0xFFFF00ull));
    expect("64 compute programs", past(set_of, 64, VK_CSF_SETS, VK_CSF_WAYS), 1);
    /* every set index is a set of the table */
    for (u32 i = 0; i < 4096; i++)
        if (vk_sha_set(next()) >= sets || vk_csf_set(next()) >= VK_CSF_SETS) {
            printf("a set index past the table\n");
            failures++;
            break;
        }
    printf("vk lookup ways: %d failures\n", failures);
    return failures != 0;
}
