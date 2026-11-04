/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* shader_shared_reach and shader_shared_fit: how far a compute program's shared memory accesses reach, and the size
   it declares on a device that gives a block less than it asks for (32 KiB here, an Adreno's, for a launch asking
   for 48 KiB). Each program's answer is worked out by hand in its comment. */
#include "nx/gpu/shader/shader.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCHED       0x001F8000FC0007E0ull
#define EXIT        0xE30000000007000Full
#define SYNC        0xF0F800000007000Full
#define CAL_NEXT    0xE26000000000000Full /* CAL to the next instruction */
#define RZ          255u
#define P0          0u /* a guard on predicate 0, which nothing here sets: the walk takes both ways */
#define DEVICE_MAX  32768u
#define LAUNCH_ASKS 49152u

static int failures;
static void check(int condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static u64 guarded(u64 w, u32 guard) { return (w & ~0xF0000ull) | (u64)guard << 16; }
static u64 s2r(u32 d, u32 sr) { return 0xF0C8000000070000ull | (u64)sr << 20 | d; }
static u64 mov32i(u32 d, u32 v) { return 0x010000000007F000ull | (u64)v << 20 | d; }
static u64 iadd32i(u32 d, u32 a, u32 v) { return 0x1C00000000070000ull | (u64)v << 20 | (u64)a << 8 | d; }
static u64 shl_i(u32 d, u32 a, u32 s) { return 0x3848000000070000ull | (u64)s << 20 | (u64)a << 8 | d; }
/* XMAD with a 16-bit immediate: d = (A's half * imm) << (psl ? 16 : 0) + c */
static u64 xmad_i(u32 d, u32 a, u32 imm, u32 c, int psl, int a_high) {
    return 0x3600000000070000ull | d | (u64)a << 8 | (u64)imm << 20 | (u64)psl << 36 | (u64)c << 39 | (u64)a_high << 53;
}
/* 32-bit LDS and STS at [a + offset] */
static u64 lds(u32 d, u32 a, u32 offset) {
    return 0xEF4C000000070000ull | (u64)(offset & 0xFFFFFF) << 20 | (u64)a << 8 | d;
}
static u64 sts(u32 d, u32 a, u32 offset) {
    return 0xEF5C000000070000ull | (u64)(offset & 0xFFFFFF) << 20 | (u64)a << 8 | d;
}
/* instruction k's word: after the 80-byte header (10 words) a scheduling word leads every three */
static u32 word_of(u32 k) { return 11 + k + k / 3; }
/* a branch at instruction from to instruction to: the offset counts bytes from the next word */
static u64 bra(u32 from, u32 to, u32 guard) {
    i32 bytes = ((i32)word_of(to) - (i32)word_of(from) - 1) * 8;
    return 0xE24000000000000Full | (u64)((u32)bytes & 0xFFFFFF) << 20 | (u64)guard << 16;
}
static u64 ssy(u32 from, u32 to) {
    i32 bytes = ((i32)word_of(to) - (i32)word_of(from) - 1) * 8;
    return 0xE290000000000000ull | (u64)((u32)bytes & 0xFFFFFF) << 20;
}

/* the program as guest memory holds it: 80 zero bytes, the instructions with their scheduling words, an EXIT, and
   zeros after it */
static u8* program(const u64* insn, u32 count, u32* bytes) {
    u32 words = word_of(count) + 8;
    u8* guest = calloc(words, 8);
    if (!guest) return NULL;
    for (u32 k = 0; k <= count; k++) {
        if (k % 3 == 0) snx_st_le64(guest + (size_t)(word_of(k) - 1) * 8, SCHED);
        snx_st_le64(guest + (size_t)word_of(k) * 8, k < count ? insn[k] : EXIT);
    }
    *bytes = words * 8;
    return guest;
}

static u32 reach_of(const u64* insn, u32 count, u32 lx) {
    u32 bytes = 0, local[3] = {lx, 1, 1};
    u8* guest = program(insn, count, &bytes);
    u32 reach = guest ? shader_shared_reach(guest, bytes, local) : 0;
    free(guest);
    return reach;
}

static void check_reach(const u64* insn, u32 count, u32 lx, u32 want, const char* what) {
    u32 got = reach_of(insn, count, lx);
    if (got != want) fprintf(stderr, "%s: reach %u, want %u\n", what, got, want);
    check(got == want, what);
}

/* the SPIR-V declares a Workgroup array of words u32s: an OpConstant of that value an OpTypeArray takes as its length */
static int declares_words(const u32* spv, size_t count, u32 words) {
    for (size_t i = 5; i < count;) {
        u32 op = spv[i] & 0xFFFF, len = spv[i] >> 16;
        if (!len) return 0;
        if (op == 28 && i + 3 < count) { /* OpTypeArray result element length */
            u32 length_id = spv[i + 3];
            for (size_t j = 5; j < count;) {
                u32 op2 = spv[j] & 0xFFFF, len2 = spv[j] >> 16;
                if (!len2) return 0;
                if (op2 == 43 && len2 == 4 && spv[j + 2] == length_id && spv[j + 3] == words) return 1;
                j += len2;
            }
        }
        i += len;
    }
    return 0;
}

int main(void) {
    /* each of 64 threads at its index times 4: the last word starts at 252 */
    const u64 tid4[] = {s2r(0, 0x21), shl_i(0, 0, 2), sts(1, 0, 0)};
    check_reach(tid4, 3, 64, 256, "thread index times 4");
    /* the same at 0x8000 on: past 32 KiB by 256 bytes */
    const u64 tid4_high[] = {s2r(0, 0x21), shl_i(0, 0, 2), sts(1, 0, 0x8000)};
    check_reach(tid4_high, 3, 64, 0x8000 + 256, "thread index times 4 from 32 KiB");
    /* one word at a constant address */
    const u64 constant[] = {sts(1, RZ, 0x9000)};
    check_reach(constant, 1, 64, 0x9004, "constant address");
    /* an address loaded from shared memory itself: any */
    const u64 loaded[] = {lds(0, RZ, 0), sts(1, 0, 0)};
    check_reach(loaded, 2, 64, SH_SHARED_ANY, "address read from memory");
    /* a counter a loop adds 4 to: it grows past every bound */
    const u64 loop[] = {mov32i(0, 0), iadd32i(0, 0, 4), sts(1, 0, 0), bra(3, 1, P0)};
    check_reach(loop, 4, 64, SH_SHARED_ANY, "loop counter");
    /* a predicated write keeps the old value too: 0x200 or 0x100, not 0x100 alone */
    const u64 predicated[] = {mov32i(0, 0x200), guarded(mov32i(0, 0x100), P0), sts(1, 0, 0)};
    check_reach(predicated, 3, 64, 0x204, "predicated write joins");
    /* a branch past the write: 0 (registers start at 0) or 0x400 */
    const u64 skipped[] = {bra(0, 2, P0), mov32i(0, 0x400), sts(1, 0, 0)};
    check_reach(skipped, 3, 64, 0x404, "a branch around the write");
    /* if and else joined by SSY and SYNC: 0x40 or 0x80 where they meet */
    const u64 if_else[] = {ssy(0, 6), bra(1, 4, P0), mov32i(0, 0x40), SYNC, mov32i(0, 0x80), SYNC, sts(1, 0, 0)};
    check_reach(if_else, 7, 64, 0x84, "SSY and SYNC");
    /* a thread index times 160 as two XMADs (the low half's product, then the high half's shifted up): 255 * 160 + 152
       starts the last word, as one game's 256-thread program writes 40960 bytes on the console */
    const u64 stride160[] = {s2r(20, 0x21), xmad_i(22, 20, 160, RZ, 0, 0), xmad_i(22, 20, 160, 22, 1, 1),
                             sts(1, 22, 152)};
    check_reach(stride160, 4, 256, 255 * 160 + 152 + 4, "a 32-bit by 16-bit XMAD product");
    const u64 none[] = {mov32i(0, 1)};
    check_reach(none, 1, 64, 0, "no shared access");
    /* a call, which the walk doesn't follow: no bound for an access, none needed without one */
    const u64 call_store[] = {CAL_NEXT, sts(1, RZ, 0)}, call_only[] = {CAL_NEXT, mov32i(0, 1)};
    check_reach(call_store, 2, 64, SH_SHARED_ANY, "a call before an access");
    check_reach(call_only, 2, 64, 0, "a call and no shared access");

    /* the size a launch asking for 48 KiB gets on a device that gives 32 KiB */
    {
        u32 bytes = 0, size = 0, reach = 0, local[3] = {64, 1, 1};
        u8* guest = program(tid4, 3, &bytes);
        check(guest && shader_shared_fit(guest, bytes, local, 1024, DEVICE_MAX, &size, &reach) && size == 1024 &&
                  reach == 0,
              "a launch within the device's size keeps it");
        check(guest && shader_shared_fit(guest, bytes, local, LAUNCH_ASKS, DEVICE_MAX, &size, &reach) &&
                  size == DEVICE_MAX && reach == 256,
              "accesses below the device's size: it gets the device's size");
        /* translated with what it gets: an array of 8192 words, which the device can make */
        ShaderInfo info = {0};
        size_t words = 0;
        info.stage = ST_CS;
        info.cs_local[0] = 64;
        info.cs_local[1] = info.cs_local[2] = 1;
        info.shared_size = (int)size;
        u32* spv = guest ? shader_to_spirv(guest, bytes, ST_CS, &info, &words, SH_FTZ_EMULATE, 1, 1, 1) : NULL;
        if (!spv || info.unsupported) fprintf(stderr, "translation: %s\n", info.first_unsupported);
        check(spv && !info.unsupported && declares_words(spv, words, DEVICE_MAX / 4),
              "translated with the device's size: a Workgroup array of 8192 words");
        free(spv);
        free(guest);
        guest = program(tid4_high, 3, &bytes);
        check(guest && !shader_shared_fit(guest, bytes, local, LAUNCH_ASKS, DEVICE_MAX, &size, &reach) &&
                  reach == 0x8100,
              "accesses past the device's size: left out, with how far they reach");
        free(guest);
        guest = program(loaded, 2, &bytes);
        check(guest && !shader_shared_fit(guest, bytes, local, LAUNCH_ASKS, DEVICE_MAX, &size, &reach) &&
                  reach == SH_SHARED_ANY,
              "accesses with no bound: left out");
        free(guest);
    }
    if (failures) {
        fprintf(stderr, "%d shared reach checks failed\n", failures);
        return 1;
    }
    puts("shared memory reach: indices, constants, loads, loops, predicates, branches, SSY/SYNC, XMAD products and the "
         "device's size checked");
    return 0;
}
