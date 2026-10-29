/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gpu/shipped_pass.c's walk over a RomFS and a segment made here, with a sink that records what it is handed: the
   programs counted then handed on, the files and the whole marked read, a later walk that reads none of them again,
   a later build's walk that reads again the files with programs and not the one found to hold none, and what keeps a
   mark from being written (a program not handled for want of memory, a stop) or doesn't (a program that fails to
   translate) */
#include "nx/gpu/shipped_pass.h"
#include "nx/gpu/shader/shipped.h"
#include "nx/gpu/shader/shader.h"
#include "nx/hle/filesystem/fs.h"
#include "nx/runtime/loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void put32(u8* p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}
/* a program at d + at: the wrapper, then the SPH's first word (type 2 pixel, version 3, shader type 5) */
static void program(u8* d, size_t at) {
    put32(d + at, 0x12345678u);
    put32(d + at + 0x30, 2u | 3u << 5 | 5u << 10);
}

/* the RomFS: its files, each at a place of its own in the image (the n-th at n MiB) */
#define FILES 3
static struct {
    const char* name;
    u8 bytes[0x400];
    u64 size;
} g_files[FILES];
static int g_peeks[FILES]; /* the reads of each */
int romfs_files(RomfsEach each, void* ctx) {
    for (u64 i = 0; i < FILES; i++)
        if (each(ctx, g_files[i].name, i << 20, g_files[i].size)) return 1;
    return 0;
}
int romfs_peek(u64 off, void* dst, u64 size) {
    u64 i = off >> 20, at = off & 0xFFFFF;
    if (i >= FILES || at > g_files[i].size || size > g_files[i].size - at) return -1;
    g_peeks[i]++;
    memcpy(dst, g_files[i].bytes + at, (size_t)size);
    return 0;
}
/* the executable: one writable segment with a program */
static u8 g_data[0x200];
void loader_segments(LoaderSegEach each, void* ctx) {
    each(ctx, "main.data", sizeof "main.data" - 1, g_data, 0x8000000, sizeof g_data, 1);
}

/* the sink: the ids marked (SHIP_FILE ones and SHIP_ALL's), the programs handed on, and what to answer for them */
static struct {
    u64 marked[16];
    u32 what[16];
    int nmarked, programs, answer, stop_after;
} g_sink;
static int scanned(void* ctx, u32 what, u64 id) {
    (void)ctx;
    for (int i = 0; i < g_sink.nmarked; i++)
        if (g_sink.what[i] == what && g_sink.marked[i] == id) return 1;
    return 0;
}
static void mark(void* ctx, u32 what, u64 id) {
    (void)ctx;
    if (g_sink.nmarked < 16) {
        g_sink.what[g_sink.nmarked] = what;
        g_sink.marked[g_sink.nmarked++] = id;
    }
}
static int program_handed(void* ctx, const u8* sph, size_t avail, int stage) {
    (void)ctx;
    (void)sph;
    (void)avail;
    (void)stage;
    g_sink.programs++;
    return g_sink.stop_after && g_sink.programs >= g_sink.stop_after ? SHIP_STOP : g_sink.answer;
}
static int not_stopped(void* ctx) {
    (void)ctx;
    return 0;
}
static void noted(void* ctx, const char* line, int shown) {
    (void)ctx;
    (void)line;
    (void)shown;
}
static int marks(u32 what) {
    int n = 0;
    for (int i = 0; i < g_sink.nmarked; i++) n += g_sink.what[i] == what;
    return n;
}

/* the marks of what kept, the first most of them, and no others: what a later run of another build finds (the
   SHIP_NONE ones), or a walk cut short */
static void keep(u32 what, int most) {
    int n = 0;
    for (int i = 0; i < g_sink.nmarked; i++)
        if (g_sink.what[i] == what && n < most) {
            g_sink.what[n] = what;
            g_sink.marked[n++] = g_sink.marked[i];
        }
    g_sink.nmarked = n;
}

static int walk(int count, int answer, int stop_after, ShipProgress* p) {
    static const ShipSink sink = {NULL, scanned, mark, program_handed, not_stopped, noted};
    *p = (ShipProgress){0};
    g_sink.programs = 0;
    g_sink.answer = answer;
    g_sink.stop_after = stop_after;
    return ship_walk(&sink, p, count);
}

int main(void) {
    ShipProgress p;
    /* a.bnsh with two programs, b.bin of no known kind, c.bfres skipped by its name though it holds one */
    g_files[0].name = "a.bnsh";
    memcpy(g_files[0].bytes, "BNSH", 4);
    program(g_files[0].bytes, 0x100);
    program(g_files[0].bytes, 0x200);
    g_files[0].size = 0x400;
    g_files[1].name = "b.bin";
    g_files[1].size = 0x100;
    g_files[2].name = "c.bfres";
    program(g_files[2].bytes, 0x100);
    g_files[2].size = 0x400;
    program(g_data, 0x100);
    ship_snapshot();

    /* a program not handled for want of memory: its source isn't marked, and neither is the whole */
    CHECK(walk(1, SHIP_LACKED, 0, &p) == -1);
    CHECK(g_sink.programs == 3 && p.programs == 3 && p.programs_done == 3 && p.files == 4 && p.files_seen == 4);
    CHECK(marks(SHIP_FILE) == 0 && marks(SHIP_ALL) == 0);
    /* stopped by the sink: nothing more handed on, the whole not marked */
    CHECK(walk(1, SHIP_NEW, 2, &p) == 1 && g_sink.programs == 2 && marks(SHIP_ALL) == 0);
    g_sink.nmarked = 0;
    /* translated, one failing as it would on every run: the sources with programs and the whole marked read for this
       build, b.bin for every build: nothing in it */
    CHECK(walk(1, SHIP_FAILED, 0, &p) == 0 && g_sink.programs == 3);
    CHECK(marks(SHIP_FILE) == 2 && marks(SHIP_ALL) == 1 && marks(SHIP_NONE) == 1);
    /* a later walk: the whole was read, so nothing is read again */
    CHECK(walk(1, SHIP_NEW, 0, &p) == 0 && g_sink.programs == 0 && p.files == 0);
    /* another build's walk, which has the SHIP_NONE marks alone: the sources with programs read again, b.bin not */
    keep(SHIP_NONE, FILES);
    memset(g_peeks, 0, sizeof g_peeks);
    CHECK(walk(1, SHIP_NEW, 0, &p) == 0 && g_sink.programs == 3 && marks(SHIP_ALL) == 1);
    CHECK(g_peeks[0] > 0 && g_peeks[1] == 0 && p.files_seen == 4);
    /* without counting first, a source not marked read is handed on as it is read, and the rest aren't again */
    keep(SHIP_FILE, 1); /* the segment's alone, the first marked: a.bnsh is read again */
    CHECK(walk(0, SHIP_NEW, 0, &p) == 0 && g_sink.programs == 2 && p.programs == 0 && marks(SHIP_ALL) == 1);
    CHECK(g_peeks[2] == 0); /* c.bfres, by its name, on no walk */
    ship_release();

    printf("shipped_pass: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
