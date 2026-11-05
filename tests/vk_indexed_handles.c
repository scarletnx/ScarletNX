/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* First-seen descriptor IDs against the old linear scan, including ordinary handle zero, separate texture kinds,
   null descriptors inserted outside the lookup, hash collisions and a full table. The optional benchmark measures
   CPU handle lookup only, with 256 lookups per table and deterministic generated handles. */
#include "nx/gpu/vk/indexed_handles.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CAPACITY 256u
typedef struct {
    uint32_t handles[CAPACITY];
    uint8_t kinds[CAPACITY];
    uint32_t used;
} Entries;
static struct {
    uint64_t rng, checksum;
} g_test = {0x6a09e667f3bcc909ull, 0};

static uint32_t random_word(void) {
    g_test.rng ^= g_test.rng >> 12;
    g_test.rng ^= g_test.rng << 25;
    g_test.rng ^= g_test.rng >> 27;
    return (uint32_t)(g_test.rng * 0x2545F4914F6CDD1Dull >> 32);
}
static uint32_t linear_find(const Entries* e, uint32_t handle, uint8_t kind) {
    uint32_t id = 0;
    while (id < e->used && (e->handles[id] != handle || e->kinds[id] != kind)) id++;
    return id;
}
static void store(Entries* e, uint32_t id, uint32_t handle, uint8_t kind) {
    if (id == e->used && id < CAPACITY) {
        e->handles[id] = handle;
        e->kinds[id] = kind;
        e->used++;
    }
}
static void compare(Entries* expected, Entries* actual, uint16_t* slots, uint32_t handle, uint8_t kind,
                    uint32_t limit) {
    uint32_t ref = linear_find(expected, handle, kind);
    uint32_t got =
        vk_indexed_handle_find(slots, CAPACITY, actual->handles, actual->kinds, actual->used, handle, kind, limit);
    assert(got == ref);
    store(expected, ref, handle, kind);
    store(actual, got, handle, kind);
    assert(!memcmp(expected, actual, sizeof *expected));
}
static uint32_t bucket(uint32_t handle, uint8_t kind) {
    uint64_t h = ((uint64_t)handle | (uint64_t)kind << 32) * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(h ^ (h >> 32)) & (2 * CAPACITY - 1);
}
static void correctness(uint32_t limit) {
    Entries expected = {0}, actual = {0};
    uint16_t slots[2 * CAPACITY + 1] = {0};
    slots[2 * CAPACITY] = 0xdead;
    compare(&expected, &actual, slots, 0, 0, limit);
    /* The renderer reserves null IDs in its descriptor array without adding lookup keys. */
    store(&expected, expected.used, 0, 255);
    store(&actual, actual.used, 0, 255);
    compare(&expected, &actual, slots, 0, 1, limit);
    compare(&expected, &actual, slots, 0, 0, limit);
    compare(&expected, &actual, slots, UINT32_MAX, 7, limit);
    /* All these different handles deliberately enter one hash bucket. */
    uint32_t collisions = 0;
    for (uint32_t h = 1; collisions < 64; h++)
        if (bucket(h, 3) == bucket(0, 3)) {
            compare(&expected, &actual, slots, h, 3, limit);
            collisions++;
        }
    for (uint32_t i = 0; i < 10000; i++) {
        uint32_t r = random_word();
        compare(&expected, &actual, slots, r & 63, (uint8_t)(r >> 8 & 3), limit);
    }
    for (uint32_t i = 0; actual.used < CAPACITY; i++) compare(&expected, &actual, slots, 0x80000000u + i, 5, limit);
    compare(&expected, &actual, slots, 0x76543210u, 17, limit);
    assert(actual.used == CAPACITY && slots[2 * CAPACITY] == 0xdead);
    /* A full table must continue finding all existing keys, even after rejecting a new one. */
    for (uint32_t i = 0; i < actual.used; i++)
        if (actual.kinds[i] != 255) compare(&expected, &actual, slots, actual.handles[i], actual.kinds[i], limit);
}

static uint64_t workload_linear(const uint32_t input[CAPACITY], uint32_t tables) {
    uint64_t sum = 0;
    for (uint32_t t = 0; t < tables; t++) {
        Entries e = {0};
        uint32_t salt = t * 0x85ebca6bu;
        for (uint32_t i = 0; i < CAPACITY; i++) {
            uint32_t handle = input[i] ^ salt;
            uint32_t id = linear_find(&e, handle, 0);
            store(&e, id, handle, 0);
            sum += id;
        }
    }
    return sum;
}
static uint64_t workload(const uint32_t input[CAPACITY], int limit, uint32_t tables) {
    if (limit < 0) return workload_linear(input, tables);
    uint64_t sum = 0;
    for (uint32_t t = 0; t < tables; t++) {
        Entries e = {0};
        uint16_t slots[2 * CAPACITY] = {0};
        uint32_t salt = t * 0x85ebca6bu;
        for (uint32_t i = 0; i < CAPACITY; i++) {
            uint32_t handle = input[i] ^ salt;
            uint32_t id =
                vk_indexed_handle_find(slots, CAPACITY, e.handles, e.kinds, e.used, handle, 0, (uint32_t)limit);
            store(&e, id, handle, 0);
            sum += id;
        }
    }
    return sum;
}
static void benchmark(uint32_t seed) {
    static const uint32_t distinct[] = {1, 16, 256};
    static const int limits[] = {-1, 0, 4, 8, 16, 32};
    uint32_t jobs[18];
    for (uint32_t i = 0; i < 18; i++) jobs[i] = i;
    g_test.rng += seed;
    for (uint32_t i = 18; i > 1; i--) {
        uint32_t j = random_word() % i, v = jobs[i - 1];
        jobs[i - 1] = jobs[j];
        jobs[j] = v;
    }
    const uint32_t tables = 20000;
    for (uint32_t job = 0; job < 18; job++) {
        uint32_t unique = distinct[jobs[job] / 6];
        int limit = limits[jobs[job] % 6];
        uint32_t input[CAPACITY];
        for (uint32_t i = 0; i < CAPACITY; i++) input[i] = (i % unique) * 0x9e3779b1u;
        g_test.checksum += workload(input, limit, 64);
        clock_t start = clock();
        uint64_t sum = workload(input, limit, tables);
        clock_t ticks = clock() - start;
        uint64_t expected = (uint64_t)tables * (CAPACITY / unique) * unique * (unique - 1) / 2;
        assert(sum == expected);
        g_test.checksum += sum;
        printf("INDEXED_HANDLES_BENCH unique=%u linear_limit=%d tables=%u lookups=%u ns=%llu exact=1\n", unique, limit,
               tables, CAPACITY, (unsigned long long)((uint64_t)ticks * 1000000000ull / CLOCKS_PER_SEC));
    }
    printf("INDEXED_HANDLES_CHECKSUM %llu\n", (unsigned long long)g_test.checksum);
}
int main(int argc, char** argv) {
    static const uint32_t limits[] = {0, 4, 8, 16, 32, CAPACITY};
    for (uint32_t i = 0; i < sizeof limits / sizeof *limits; i++) correctness(limits[i]);
    if (argc >= 2 && !strcmp(argv[1], "--bench")) benchmark(argc >= 3 ? (uint32_t)strtoul(argv[2], NULL, 10) : 0);
    puts("PASS indexed handles: first-seen IDs, zero, kinds, null slots, collisions and capacity");
    return 0;
}
