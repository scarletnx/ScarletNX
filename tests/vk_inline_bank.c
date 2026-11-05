/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The rejected CPU-shadow experiment, with a staging stand-in. No Vulkan or game data. */
#include "support/inline_bank.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define BANK0  0x650u
#define BANK2  0xA00u
#define STAGED 0x1000u

typedef struct {
    const uint8_t* source;
    uint64_t generation, frame;
    uint32_t size;
    uint8_t bytes[STAGED];
} Upload;
typedef struct {
    Upload slot[2];
    VkInlineBankVersion bank2;
    uint8_t shadow[BANK2];
    uint64_t copied, uploads;
} Fixture;

static void get(Fixture* f, uint32_t slot, uint32_t kind, const uint8_t* source, uint32_t source_size, uint32_t size,
                uint64_t generation, uint64_t frame, uint32_t owner, int baseline) {
    assert(slot < 2 && size <= STAGED && source_size <= BANK2);
    if (!baseline && kind == 2)
        generation = vk_inline_bank_version(&f->bank2, f->shadow, source, source_size, generation, owner);
    Upload* e = &f->slot[slot];
    if (e->source == source && e->generation == generation && e->frame == frame && e->size >= size) return;
    uint32_t n = size < source_size ? size : source_size;
    memcpy(e->bytes, source, n);
    memset(e->bytes + n, 0, size - n);
    e->source = source;
    e->size = size;
    e->generation = generation;
    e->frame = frame;
    f->uploads++;
    f->copied += size;
}

static void correctness(void) {
    static Fixture f;
    uint8_t source[BANK2], other[BANK2];
    for (uint32_t i = 0; i < BANK2; i++) source[i] = (uint8_t)i;
    memcpy(other, source, BANK2);
    get(&f, 0, 2, source, BANK2, 32, 1, 1, 1, 0);
    assert(f.uploads == 1 && !memcmp(f.slot[0].bytes, source, 32));
    get(&f, 0, 2, source, BANK2, 32, 2, 1, 1, 0);
    assert(f.uploads == 1); /* bank 0 changed, bank 2 did not */
    get(&f, 0, 2, source, BANK2, 16, 2, 1, 1, 0);
    assert(f.uploads == 1); /* a smaller requested prefix can reuse the larger upload */
    get(&f, 0, 2, source, BANK2, BANK2, 2, 1, 1, 0);
    assert(f.uploads == 2 && !memcmp(f.slot[0].bytes, source, BANK2));
    source[BANK2 - 1] ^= 1;
    get(&f, 0, 2, source, BANK2, BANK2, 3, 1, 1, 0);
    assert(f.uploads == 3 && !memcmp(f.slot[0].bytes, source, BANK2));
    source[0] ^= 1;
    source[0] ^= 1;
    get(&f, 0, 2, source, BANK2, BANK2, 4, 1, 1, 0);
    assert(f.uploads == 3); /* a value restored before the draw has identical bytes */
    get(&f, 0, 2, source, BANK2, BANK2, 4, 2, 1, 0);
    assert(f.uploads == 4); /* a new submission must stage again */
    get(&f, 0, 2, other, BANK2, BANK2, 4, 2, 2, 0);
    assert(f.uploads == 5 && !memcmp(f.slot[0].bytes, other, BANK2));
    other[17] ^= 1;
    get(&f, 0, 2, other, BANK2, BANK2, 4, 2, 3, 0);
    assert(f.uploads == 6 && !memcmp(f.slot[0].bytes, other, BANK2)); /* owner change cannot take the fast path */
    get(&f, 0, 2, other, BANK2, BANK2 + 32, 4, 2, 3, 0);
    assert(f.uploads == 7 && !memcmp(f.slot[0].bytes, other, BANK2));
    for (uint32_t i = BANK2; i < BANK2 + 32; i++) assert(f.slot[0].bytes[i] == 0);
    get(&f, 1, 2, other, BANK2, BANK2, 4, 2, 3, 0);
    assert(f.uploads == 8 && !memcmp(f.slot[1].bytes, other, BANK2)); /* another slot needs its own upload */
    get(&f, 0, 0, source, BANK0, BANK0, 5, 2, 3, 0);
    get(&f, 0, 2, other, BANK2, BANK2, 5, 2, 3, 0);
    assert(f.uploads == 10 && !memcmp(f.slot[0].bytes, other, BANK2)); /* bank swaps */
    VkInlineBankVersion version = {0};
    uint8_t guarded[BANK2 + 2];
    memset(guarded, 0xA5, sizeof guarded);
    vk_inline_bank_version(&version, guarded + 1, other, BANK2, 1, 1);
    assert(guarded[0] == 0xA5 && guarded[BANK2 + 1] == 0xA5);
    uint64_t prior = version.version;
    vk_inline_bank_version(&version, guarded + 1, other, 16, 2, 1);
    assert(version.version != prior && !memcmp(guarded + 1, other, 16));
    prior = version.version;
    vk_inline_bank_version(&version, guarded + 1, other, BANK2, 3, 1);
    assert(version.version != prior && !memcmp(guarded + 1, other, BANK2));
    memset(&f, 0, sizeof f);
    get(&f, 0, 2, source, BANK2, 32, 1, 1, 1, 0);
    source[BANK2 - 1] ^= 1;
    get(&f, 0, 2, source, BANK2, 32, 2, 1, 1, 0);
    get(&f, 0, 2, source, BANK2, BANK2, 2, 1, 1, 0);
    assert(f.uploads == 3 && !memcmp(f.slot[0].bytes, source, BANK2));
    VkInlineBankVersion stages[5] = {0};
    uint8_t shadows[5][BANK2];
    for (uint32_t i = 0; i < 5; i++) vk_inline_bank_version(&stages[i], shadows[i], source, BANK2, 1, 1);
    prior = stages[3].version;
    source[42] ^= 1;
    vk_inline_bank_version(&stages[3], shadows[3], source, BANK2, 2, 1);
    assert(stages[3].version != prior && !memcmp(shadows[3], source, BANK2));
    for (uint32_t i = 0; i < 5; i++)
        if (i != 3) assert(stages[i].version == 1 && shadows[i][42] != source[42]);
}

static void benchmark(int baseline) {
    const uint32_t operations = 1000000;
    static const char* const names[] = {"inline-bank0-changing", "inline-bank2-changing", "inline-stable-generation"};
    for (uint32_t workload = 0; workload < 3; workload++) {
        static Fixture f;
        memset(&f, 0, sizeof f);
        uint8_t bank0[BANK0] = {0}, bank2[BANK2] = {0};
        clock_t start = clock();
        for (uint32_t i = 1; i <= operations; i++) {
            uint64_t generation = workload == 2 ? 1 : i;
            uint8_t* changing = workload == 1 ? bank2 : bank0;
            if (workload != 2) memcpy(changing + 16, &i, sizeof i);
            get(&f, 0, 0, bank0, BANK0, BANK0, generation, 1, 1, baseline);
            get(&f, 1, 2, bank2, BANK2, BANK2, generation, 1, 1, baseline);
        }
        clock_t ticks = clock() - start;
        int exact = !memcmp(f.slot[0].bytes, bank0, BANK0) && !memcmp(f.slot[1].bytes, bank2, BANK2);
        assert(exact);
        uint64_t expected = workload == 2                ? BANK0 + BANK2
                            : !baseline && workload == 0 ? (uint64_t)operations * BANK0 + BANK2
                                                         : (uint64_t)operations * (BANK0 + BANK2);
        assert(f.copied == expected);
        printf("GPU_HOTPATH_BENCH workload=%s operations=%u ns=%llu exact=1 copied_bytes=%llu uploads=%llu\n",
               names[workload], operations, (unsigned long long)((uint64_t)ticks * 1000000000ull / CLOCKS_PER_SEC),
               (unsigned long long)f.copied, (unsigned long long)f.uploads);
    }
}

int main(int argc, char** argv) {
    correctness();
    int bench = 0, baseline = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bench"))
            bench = 1;
        else if (!strcmp(argv[i], "--baseline"))
            baseline = 1;
        else
            return 2;
    }
    if (bench) benchmark(baseline);
    puts("PASS inline banks: CPU shadows, source/owner changes, size growth, padding, bank swaps and submit lifetime");
    return 0;
}
