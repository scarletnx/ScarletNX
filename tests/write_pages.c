/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The per-page write generations behind gpu_mem_dirty_since: exact per page, at any distance and any range size */
#include "nx/gpu/maxwell/write_pages.h"
#include <stdio.h>
#include <stdlib.h>

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

/* the runs write_pages_runs hands out, in order */
typedef struct {
    u64 at[160], n[160];
    u32 count;
} Runs;
static void take_run(void* ctx, u64 at, u64 n) {
    Runs* r = ctx;
    if (r->count < 160) r->at[r->count] = at, r->n[r->count] = n;
    r->count++;
}
/* write_pages_runs over bytes [lo, hi) of pages first to last: each byte on a page newer than since in exactly one run,
   no other byte in any, the runs in order and apart (pages that touch make one run) */
static int runs_match(const WritePages* t, const u64* pages, u64 base, u64 first, u64 last, u64 lo, u64 hi, u64 since) {
    Runs r = {{0}, {0}, 0};
    write_pages_runs(t, since, lo, hi - lo, take_run, &r);
    if (r.count > 160) return 0;
    for (u32 k = 0; k < r.count; k++)
        if (!r.n[k] || r.at[k] < lo || r.at[k] + r.n[k] > hi || (k && r.at[k - 1] + r.n[k - 1] >= r.at[k])) return 0;
    for (u64 i = first; i <= last; i++) {
        u64 a = base + i * 4096, b = a + 4096, in = 0;
        if (a < lo) a = lo;
        if (b > hi) b = hi;
        for (u32 k = 0; k < r.count; k++)
            if (r.at[k] < b && a < r.at[k] + r.n[k])
                in += (r.at[k] + r.n[k] < b ? r.at[k] + r.n[k] : b) - (r.at[k] > a ? r.at[k] : a);
        if (in != (pages[i] > since ? b - a : 0)) return 0;
    }
    return 1;
}

static void ranges(void) {
    static WritePages table;
    /* Include a leaf boundary and several group boundaries, then compare both clean and dirty subranges against a
       page-by-page reference. The writer must not mark neighbors or skip the first or last partial page. */
    const u64 base = 0x600000000ull - 8 * 4096;
    u64 pages[144] = {0};
    u64 random = 1;
    for (u64 generation = 1; generation <= 256; generation++) {
        random = random * 6364136223846793005ull + 1442695040888963407ull;
        u64 first = (random >> 32) % 144;
        u64 count = 1 + (random & 0xFFFF) % (144 - first);
        write_pages_stamp(&table, base + first * 4096 + 4095, (count - 1) * 4096 + 1, generation);
        for (u64 i = first; i < first + count; i++) pages[i] = generation;
        for (u64 begin = 0; begin < 144; begin++) {
            u64 end = begin + (generation % (144 - begin));
            for (u64 age = 0; age < 3; age++) {
                u64 since = generation - (generation < age ? generation : age);
                int expected = 0;
                for (u64 i = begin; i <= end; i++) expected |= pages[i] > since;
                check(write_pages_since(&table, since, base + begin * 4096, (end - begin + 1) * 4096) == expected,
                      "batched stamps match exact page generations across group and leaf boundaries");
                check(runs_match(&table, pages, base, begin, end, base + begin * 4096 + 100, base + end * 4096 + 4000,
                                 since),
                      "the runs of written pages are exactly the pages written since, clipped to the range");
            }
        }
    }
    check(!write_pages_since(&table, 0, base - 1, 1), "the byte before the tested ranges stays clean");
    check(!write_pages_since(&table, 0, base + sizeof pages / sizeof pages[0] * 4096, 4096),
          "the page after the tested ranges stays clean");
    write_pages_stamp(&table, 0, 0, 257);
    check(!write_pages_since(&table, 256, base, 144 * 4096), "an empty stamp changes no generation");
    write_pages_stamp(&table, ~0ull - 3, 8, 258);
    check(write_pages_since(&table, 257, base, 1), "a wrapped range conservatively stamps overflow");
    {
        Runs r = {{0}, {0}, 0};
        write_pages_runs(&table, 257, base + 5, 3 * 4096, take_run, &r);
        check(r.count == 1 && r.at[0] == base + 5 && r.n[0] == 3 * 4096,
              "past an overflow newer than since, the whole range is one run");
    }
}

int main(void) {
    ranges();
    /* a table that lasts, as the runtime's: nothing frees the leaves it makes */
    static WritePages table;
    WritePages* t = &table;
    const u64 tex = 0x500000000ull, tex_size = 16u << 20; /* a 16 MiB texture */
    u64 g;
    check(!write_pages_since(t, 0, tex, tex_size), "an empty table has no writes");
    /* constant buffer updates 256 MiB and 1 GiB away shared the texture's slots in the hashed table */
    write_pages_stamp(t, tex + (256u << 20) + 0x40, 0x100, 1);
    write_pages_stamp(t, tex - (1u << 30), 0x10000, 2);
    for (g = 3; g < 5000; g++) write_pages_stamp(t, 0x100000000ull + (g & 63) * 0x1000, 4, g);
    check(!write_pages_since(t, 0, tex, tex_size), "writes elsewhere leave a large range clean");
    write_pages_stamp(t, tex + tex_size - 4, 4, 5000);
    check(write_pages_since(t, 4999, tex, tex_size), "a write in the last page of a large range is seen");
    check(!write_pages_since(t, 5000, tex, tex_size), "a write before since is not");
    check(!write_pages_since(t, 4999, tex, tex_size - 0x1000), "a range ending before the page is clean");
    write_pages_stamp(t, tex + tex_size - 0x800, 0x1000, 5001); /* across two leaves */
    check(write_pages_since(t, 5000, tex + tex_size, 0x1000), "a write is seen in every page it covers");
    check(write_pages_since(t, 5000, tex, tex_size), "and in the leaf it starts in");
    check(!write_pages_since(t, 5000, tex, 0x1000), "an unwritten page of a written leaf is clean");
    check(!write_pages_since(t, 5001, 0, ~0ull), "the whole address space after the latest write is clean");
    check(write_pages_since(t, 5000, 0, ~0ull), "the whole address space sees the latest write");
    write_pages_stamp(t, 1ull << 40, 0x1000, 5002);
    check(write_pages_since(t, 5001, tex, 1), "a write past 2^40 reads as covering every page");
    if (!failures) puts("Write generations are kept per page");
    return failures ? 1 : 0;
}
