/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The guest pages pending compute results cover (vk/pending_pages.c), over a stand-in GMMU: GPU ranges that alias the
   same guest pages meet, unmapped GPU pages cover nothing, a result removed leaves the pages others cover, and a
   mapping changed while a result is pending counts it at its new pages */
#include "nx/gpu/vk/pending_pages.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/log.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* GPU page -> guest address | 1, for the GPU pages under 1 GiB */
#define GPU_PAGES (1u << 18)
static u64 gmmu[GPU_PAGES];

/* a mapping change ends with pend_remap, as the renderer's mapping bracket does */
static void map(u64 gva, u64 cpu, u64 size) {
    for (u64 p = gva >> 12; p < (gva + size) >> 12; p++, cpu += 0x1000) gmmu[p] = cpu | 1;
    pend_remap(gva, size);
}
static void unmap(u64 gva, u64 size) {
    for (u64 p = gva >> 12; p < (gva + size) >> 12; p++) gmmu[p] = 0;
    pend_remap(gva, size);
}
/* as nvdrv.c's: the bytes from gva on that map contiguously, at most max, and where they start */
u64 gmmu_run(u64 gva, u64 max, u64* cpu) {
    u64 page = gva >> 12, n, want;
    if (!max || page >= GPU_PAGES || !(gmmu[page] & 1)) return 0;
    *cpu = (gmmu[page] & ~0xFFFull) | (gva & 0xFFF);
    n = 0x1000 - (gva & 0xFFF);
    want = (gmmu[page] & ~0xFFFull) + 0x1000;
    while (n < max && ++page < GPU_PAGES && gmmu[page] == (want | 1)) {
        n += 0x1000;
        want += 0x1000;
    }
    return n < max ? n : max;
}
/* as nvdrv.c's index, by brute force: each GPU page showing guest bytes of [cpu, cpu + size), clipped to them */
void gmmu_aliases(u64 cpu, u64 size, void (*fn)(void* ctx, u64 gva, u64 n), void* ctx) {
    for (u64 p = 0; p < GPU_PAGES; p++) {
        u64 c = gmmu[p] & ~0xFFFull, a = c > cpu ? c : cpu, b = c + 0x1000 < cpu + size ? c + 0x1000 : cpu + size;
        if ((gmmu[p] & 1) && a < b) fn(ctx, (p << 12) + (a - c), b - a);
    }
}
void gmmu_pieces(u64 gva, u64 size, void (*fn)(void* ctx, u64 gva, u64 cpu, u64 n), void* ctx) {
    for (u64 p = gva >> 12; p <= (gva + size - 1) >> 12 && p < GPU_PAGES; p++)
        if (gmmu[p] & 1) fn(ctx, p << 12, gmmu[p] & ~0xFFFull, 0x1000);
}
/* gpu_page_check's warnings: pend_hit answered no where translating the range finds a result */
static int warnings;
void logf_(int level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    warnings += !level; /* WARN's level, not LOG's */
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(2);
}

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

int main(void) {
    /* tables that last, as the runtime's: none frees the leaves it makes */
    static PendPages tables[3];
    PendPages* t = &tables[0];
    g_settings.gpu_page_check = 1;           /* every no is checked against the translated range */
    map(0x100000, 0x80000000ull, 0x10000);   /* 16 pages */
    map(0x400000, 0x80004000ull, 0x2000);    /* aliases pages 4 and 5 of the range above */
    map(0x600000, 0x90000000ull, 0x4000000); /* 64 MiB: crosses table leaves (16 MiB of guest pages each) */
    check(!pend_hit(t, 0x100000, 0x10000) && pend_empty(t), "an empty table covers nothing");

    pend_add(t, 0x104000, 0x2000, 1);
    check(pend_hit(t, 0x105FFF, 1), "the last byte of a result's range");
    check(!pend_hit(t, 0x106000, 0x1000) && !pend_hit(t, 0x100000, 0x4000), "the pages beside it");
    check(pend_hit(t, 0x401000, 4), "a GPU range that aliases its guest pages");
    check(!pend_empty(t), "a result's pages are counted");

    pend_add(t, 0x400000, 0x1000, 1); /* the same guest page through the alias */
    pend_add(t, 0x104000, 0x2000, -1);
    check(pend_hit(t, 0x104000, 1) && !pend_hit(t, 0x105000, 1), "removing one result leaves another's page");
    pend_add(t, 0x400000, 0x1000, -1);
    check(pend_empty(t) && !pend_hit(t, 0x100000, 0x10000), "every result removed leaves nothing");

    /* a hole in the GPU range: the unmapped page covers nothing, the pages after it do */
    unmap(0x108000, 0x1000);
    pend_add(t, 0x107000, 0x3000, 1);
    check(!pend_hit(t, 0x108000, 0x1000) && pend_hit(t, 0x109000, 1), "a result over an unmapped GPU page");
    pend_add(t, 0x107000, 0x3000, -1);
    check(pend_empty(t), "and its removal");

    /* a large range: the first group and leaf are skipped, a page at the far end is found */
    pend_add(t, 0x600000 + 0x3FFF000, 0x1000, 1);
    check(pend_hit(t, 0x600000, 0x4000000), "the last page of 64 MiB");
    check(!pend_hit(t, 0x600000, 0x3FFF000), "64 MiB just before it");
    check(pend_hit(t, 0x600000 + 0x3FC0000, 0x40000) && !pend_hit(t, 0x600000 + 0x3F80000, 0x3F000),
          "the group of 64 pages it is in, and the one before");
    pend_add(t, 0x600000 + 0x3FFF000, 0x1000, -1);

    /* past the table's 2^40 bytes of guest pages: read as everywhere until removed */
    map(0x200000, 0x20000000000ull, 0x1000);
    pend_add(t, 0x200000, 0x1000, 1);
    check(pend_hit(t, 0x600000, 0x1000), "a page past 2^40 covers every page");
    pend_add(t, 0x200000, 0x1000, -1);
    check(pend_empty(t) && !pend_hit(t, 0x600000, 0x1000), "until it is removed");

    /* by GPU address, no translation: the alias above doesn't meet it, the page itself does */
    pend_add_va(t, 0x104000, 0x2000, 1);
    check(pend_hit_va(t, 0x105FFF, 1) && !pend_hit_va(t, 0x106000, 0x1000) && !pend_hit_va(t, 0x400000, 0x2000),
          "pages by GPU address");
    pend_add_va(t, 0x104000, 0x2000, -1);
    check(pend_empty(t), "and their removal");

    /* a table counted in another as well: the other sees what either holds, and empties when both do */
    {
        PendPages *u = &tables[1], *a = &tables[2];
        a->also = u;
        pend_add(a, 0x104000, 0x1000, 1);
        pend_add(u, 0x109000, 0x1000, 1);
        check(pend_hit(u, 0x104000, 1) && pend_hit(u, 0x109000, 1) && !pend_hit(a, 0x109000, 1),
              "a table's pages in the one it is counted in too");
        pend_add(a, 0x104000, 0x1000, -1);
        check(!pend_hit(u, 0x104000, 1) && pend_hit(u, 0x109000, 1) && pend_empty(a), "and its removal there");
        pend_add(u, 0x109000, 0x1000, -1);
        check(pend_empty(u), "the other empty once both are");
    }

    /* a mapping made while a result is pending shows it too, and loses it when the result is removed; the result's
       own pages stay until then, as the renderer lands a result before a change over its pages */
    pend_add(t, 0x104000, 0x2000, 1);
    map(0x800000, 0x80005000ull, 0x3000); /* its second page, and two beside it */
    check(pend_hit(t, 0x800000, 1) && !pend_hit(t, 0x801000, 0x2000), "a new mapping of a pending page");
    unmap(0x400000, 0x2000);
    check(!pend_hit(t, 0x400000, 0x2000) && pend_hit(t, 0x105000, 1), "an alias unmapped");
    map(0x400000, 0x80004000ull, 0x2000);
    check(pend_hit(t, 0x400000, 1) && pend_hit(t, 0x401000, 1), "and mapped again");
    pend_add(t, 0x104000, 0x2000, -1);
    check(pend_empty(t) && !pend_hit(t, 0x800000, 0x3000) && !pend_hit(t, 0x400000, 0x2000), "every alias empty");
    unmap(0x800000, 0x3000);

    /* a range to the top of the address space ends there */
    check(!pend_hit(t, ~0ull - 0xFFF, ~0ull), "a range that would wrap");
    check(!warnings, "no answer of the GPU pages missed a result");
    if (failures) return 1;
    printf("pending pages: aliases, holes, large ranges and removals\n");
    return 0;
}
