/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native nn::prepo (src/nx/nn/prepo.c) on a stand-in CPU, in the report layouts of SDK 7.7.0 and SDK 20: Save
   refuses an event id that is empty, too long or has a character past 0-9, a-z and _, and leaves the report then;
   a save starts the report's buffer again (de 00 00, or nothing in a buffer under 3 bytes); and SDK 20 lets 900 saves
   through in any 90 minutes, a save past them succeeding with the report left as it is */
#include "nx/nn/prepo.c"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

Hle g_hle;
static u64 test_ns = 1000000000ull;
u64 guest_ns(void) { return test_ns; }
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}
void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }

static CPU cpu;
static u8 mem[0x1000];
#define REPORT 0x100u
#define BUFFER 0x400u

/* a report with an event id and a used buffer of 10 bytes */
static void report(const HookLayout_nn_prepo_PlayReport* L, const char* id, u64 capacity) {
    memset(mem + REPORT, 0, 0x40);
    strcpy((char*)mem + REPORT, id);
    hook_put_addr(&cpu, L->buffer, REPORT, BUFFER);
    hook_put_u64(&cpu, L->capacity, REPORT, capacity);
    hook_put_u64(&cpu, L->used, REPORT, 10);
    memset(mem + BUFFER, 0xee, 16);
}
static int cleared(const HookLayout_nn_prepo_PlayReport* L) {
    return mem[BUFFER] == 0xde && mem[BUFFER + 1] == 0 && mem[BUFFER + 2] == 0 &&
           hook_get_u64(&cpu, L->used, REPORT) == 3;
}

int main(void) {
    CPU* c = &cpu;
    cpu.mem = mem;
    nnprepo_init();
    static const HookLayout_nn_prepo_PlayReport v7 = {0x38, {0x20}, {0x28}, {0x30}}, v20 = {0x40, {0x28}, {0x30}, {0x38}};
    const HookLayout_nn_prepo_PlayReport* layouts[2] = {&v7, &v20};
    for (int v = 0; v < 2; v++) {
        const HookLayout_nn_prepo_PlayReport* L = layouts[v];
        u32 (*save)(CPU*, const HookLayout_nn_prepo_PlayReport*, u64) = v ? nnprepo_save : nnprepo_save_unlimited;
        /* event ids refused, the report left */
        static const char* const bad[] = {"", "Upper", "with space", "dash-ed", "abcdefghijklmnopqrstuvwxyz_01234"};
        for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            report(L, bad[i], 0x100);
            CHECK(save(c, L, REPORT) == 0xa81 && mem[BUFFER] == 0xee && hook_get_u64(c, L->used, REPORT) == 10);
        }
        /* saved: the buffer started again */
        report(L, "abcdefghijklmnopqrstuvwxyz_0123", 0x100);
        CHECK(save(c, L, REPORT) == 0 && cleared(L));
        report(L, "event_1", 0x100);
        CHECK((v ? nnprepo_save_user(c, L, REPORT, 0x200) : nnprepo_save_user_unlimited(c, L, REPORT, 0x200)) == 0);
        CHECK(cleared(L));
        /* a buffer with no room for the map: nothing written, nothing used */
        report(L, "event_1", 2);
        CHECK(save(c, L, REPORT) == 0 && mem[BUFFER] == 0xee && hook_get_u64(c, L->used, REPORT) == 0);
    }
    CHECK(nnprepo_request_immediate_transmission(c) == 0);

    /* SDK 20's limit: the saves so far, then up to 900 in the 90 minutes; the 901st succeeds and leaves the report */
    g_hle.prepo = (HlePrepo){0};
    for (int i = 0; i < 900; i++) {
        report(&v20, "event_1", 0x100);
        CHECK(nnprepo_save(c, &v20, REPORT) == 0 && cleared(&v20));
        test_ns += 1000000000ull;
    }
    report(&v20, "event_1", 0x100);
    CHECK(nnprepo_save(c, &v20, REPORT) == 0 && mem[BUFFER] == 0xee);
    report(&v20, "Bad id", 0x100); /* the limit comes before the event id's check */
    CHECK(nnprepo_save(c, &v20, REPORT) == 0);
    test_ns += 5400ull * 1000000000ull; /* 90 minutes after the first */
    report(&v20, "event_1", 0x100);
    CHECK(nnprepo_save(c, &v20, REPORT) == 0 && cleared(&v20));
    /* SDK 7.7.0 has no limit */
    report(&v7, "event_1", 0x100);
    CHECK(nnprepo_save_unlimited(c, &v7, REPORT) == 0 && cleared(&v7));
    printf("nn_prepo: %u checks passed\n", checks);
    return 0;
}
