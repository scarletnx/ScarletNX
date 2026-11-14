/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native nn::prepo (src/nx/nn/prepo.c) on a stand-in CPU, in the report layouts of SDK 7.7.0 and SDK 20, and
   through prepo.hook's table for every SDK version read: Save
   refuses an event id that is empty, too long or has a character past 0-9, a-z and _, and leaves the report then;
   a save starts the report's buffer again (de 00 00, or nothing in a buffer under 3 bytes); and SDK 20 lets 900 saves
   through in any 90 minutes, a save past them succeeding with the report left as it is. Then a 32-bit game's table:
   the six hooks for the 32-bit SDK read and none for another, and Save on that SDK's report of 4-byte fields. Then
   AddSessionId for the SDKs read from 10: the SDK's own PlayReport::Add of 8 zero bytes under the key, below the
   caller's stack, its Result given back and the stack as it was */
#include "nx/nn/prepo.c"
#include "hle/hook/hooks.hook.inc" /* prepo.hook's table and adapters, and a 32-bit game's */
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
/* hook.c's: where Send is hooked, and the SDK's own code run in a handler's place, counted */
#define SEND_AT 0x7000u
static unsigned own_runs;
u64 hook_site(const char* symbol) { return !strcmp(symbol, PREPO_SYM_SEND) ? SEND_AT : 0; }
void hook_run_original(CPU* c, u64 addr) {
    (void)c;
    if (addr == SEND_AT) own_runs++;
}
void fatal(const char* fmt, ...) {
    fprintf(stderr, "fatal: %s\n", fmt);
    exit(1);
}
/* the SDK's PlayReport::Add(key, Any64BitId), which AddSessionId runs: where the game has it, and each call's
   arguments, its stack pointer and the 8 bytes its id points at then */
#define ADD_AT 0x7100u
static struct {
    unsigned calls;
    u64 fn, a[4], sp, id;
} added;
u64 kernel_find_symbol_exact(const char* name) { return !strcmp(name, PREPO_SYM_ADD_ID) ? ADD_AT : 0; }
void hook_guest_call(CPU* c, u64 fn, u64 a0, u64 a1, u64 a2, u64 a3) {
    added.calls++;
    added.fn = fn, added.a[0] = a0, added.a[1] = a1, added.a[2] = a2, added.a[3] = a3;
    added.sp = c->x[31];
    added.id = snx_ld_le64(c->mem + a2);
    c->x[0] = 0x2a81; /* a Result of the SDK's Add, which AddSessionId hands on */
}
void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }

static CPU cpu;
static u8 mem[0x1000];
CPU* hook_a32_cpu(SnxArmCPU* arm) { return (void)arm, &cpu; }
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
    memset(mem + 0x200, 0xee, 8); /* the status of an empty queue, in its 4 bytes */
    CHECK(nnprepo_get_transmission_status(c, 0x200) == 0 && snx_ld_le32(mem + 0x200) == 0 && mem[0x204] == 0xee);

    /* prepo.hook's own choice for each SDK version read in a game: Save with or without the limit, on the report as
       that SDK's SetBuffer lays it out (9.3, 14.3 and 15.3 among them, the last before 16's layout) */
    static const struct {
        u32 sdk, buffer;
        int limit;
    } read[] = {{0x030500, 0x20, 0}, {0x040400, 0x20, 0}, {0x060500, 0x20, 0}, {0x070700, 0x20, 0},
                {0x080201, 0x20, 0}, {0x090301, 0x20, 1}, {0x0a0400, 0x20, 1}, {0x0d0300, 0x20, 1},
                {0x0d0305, 0x20, 1}, {0x0e0300, 0x20, 1}, {0x0f0302, 0x28, 1}, {0x100200, 0x28, 1},
                {0x120302, 0x28, 1}, {0x130303, 0x28, 1}, {0x14050f, 0x28, 1}};
    static const char* const save[2] = {"_ZN2nn5prepo10PlayReport4SaveEv",
                                        "_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE"};
    static const char* const handler[2][2] = {{"nnprepo_save_unlimited", "nnprepo_save"},
                                              {"nnprepo_save_user_unlimited", "nnprepo_save_user"}};
    int first;
    for (unsigned i = 0; i < sizeof read / sizeof read[0]; i++)
        for (int user = 0; user < 2; user++) {
            u32 versions[HOOK_KEYS] = {0};
            versions[HOOK_SDK] = read[i].sdk;
            int k = hook_select(&hook_table, save[user], versions, &first);
            CHECK(k >= 0 && !strcmp(hook_handler[k], handler[user][read[i].limit]));
            memset(mem + REPORT, 0, 0x40);
            strcpy((char*)mem + REPORT, "event_1");
            snx_st_le64(mem + REPORT + read[i].buffer, BUFFER);
            snx_st_le64(mem + REPORT + read[i].buffer + 8, 0x100);
            snx_st_le64(mem + REPORT + read[i].buffer + 16, 10);
            memset(mem + BUFFER, 0xee, 16);
            g_hle.prepo = (HlePrepo){0};
            c->x[0] = REPORT, c->x[1] = 0x200, c->x[30] = 0x1234, c->pc = 0;
            hook_fn[k](c, 0);
            CHECK(c->x[0] == 0 && c->pc == 0x1234 && mem[BUFFER] == 0xde);
            CHECK(snx_ld_le64(mem + REPORT + read[i].buffer + 16) == 3);
        }
    { /* a module whose SDK the label does not give has no Save hook: its layout is not known */
        u32 versions[HOOK_KEYS] = {0};
        CHECK(hook_select(&hook_table, save[0], versions, &first) == -2);
        CHECK(hook_select(&hook_table, save[1], versions, &first) == -2);
    }
    { /* the library's start and end open and close no session: they return with nothing touched, for any SDK */
        static const char* const session[2] = {"_ZN2nn5prepo6detail3ipc10InitializeEv",
                                               "_ZN2nn5prepo6detail3ipc8FinalizeEv"};
        u32 versions[HOOK_KEYS] = {0};
        for (int i = 0; i < 2; i++) {
            int k = hook_select(&hook_table, session[i], versions, &first);
            CHECK(k >= 0);
            memset(mem + REPORT, 0xee, 0x40);
            c->x[0] = REPORT, c->x[30] = 0x4321, c->pc = 0;
            hook_fn[k](c, 0);
            CHECK(c->pc == 0x4321 && c->x[0] == REPORT && mem[REPORT] == 0xee && mem[REPORT + 0x3f] == 0xee);
        }
    }
    { /* the two that read no report hold for a module of any SDK, one whose version is unknown too */
        u32 versions[HOOK_KEYS] = {0};
        CHECK(hook_select(&hook_table, "_ZN2nn5prepo28RequestImmediateTransmissionEv", versions, &first) >= 0);
        CHECK(hook_select(&hook_table, "_ZN2nn5prepo21GetTransmissionStatusEPNS0_18TransmissionStatusE", versions,
                          &first) >= 0);
    }

    /* a 32-bit game: the six hooks hold for the 32-bit SDK read (0.12.16), and none for a module of another SDK or
       of an unknown one. Save there: the report's fields are 4 bytes each, at 0x20, 0x24 and 0x28 */
    {
        static const char* const all[6] = {save[0],
                                           save[1],
                                           "_ZN2nn5prepo28RequestImmediateTransmissionEv",
                                           "_ZN2nn5prepo21GetTransmissionStatusEPNS0_18TransmissionStatusE",
                                           "_ZN2nn5prepo6detail3ipc10InitializeEv",
                                           "_ZN2nn5prepo6detail3ipc8FinalizeEv"};
        static SnxArmCPU a;
        u32 versions[HOOK_KEYS] = {0};
        a.mem = mem, a.mem_size = sizeof mem;
        CHECK(HOOK32_COUNT == 6);
        for (int i = 0; i < 6; i++) {
            versions[HOOK_SDK] = 0;
            CHECK(hook_select(&hook32_table, all[i], versions, &first) == -2);
            versions[HOOK_SDK] = 0x010000;
            CHECK(hook_select(&hook32_table, all[i], versions, &first) == -2);
            versions[HOOK_SDK] = 0x000c10;
            CHECK(hook_select(&hook32_table, all[i], versions, &first) >= 0);
        }
        for (int user = 0; user < 2; user++) {
            int k = hook_select(&hook32_table, save[user], versions, &first);
            CHECK(!strcmp(hook32_handler[k], user ? "nnprepo_save_user32" : "nnprepo_save32"));
            CHECK(strstr(hook32_layouts[k], "nn::prepo::PlayReport32 of line"));
            memset(mem + REPORT, 0xee, 0x40);
            strcpy((char*)mem + REPORT, "event_1");
            snx_st_le32(mem + REPORT + 0x20, BUFFER);
            snx_st_le32(mem + REPORT + 0x24, 0x100);
            snx_st_le32(mem + REPORT + 0x28, 10);
            memset(mem + BUFFER, 0xee, 16);
            a.r[0] = REPORT, a.r[1] = 0x200, a.r[14] = 0x1234;
            hook32_fn[k](&a);
            CHECK(a.r[0] == 0 && a.r[15] == 0x1234 && mem[BUFFER] == 0xde && mem[BUFFER + 1] == 0 &&
                  mem[BUFFER + 2] == 0);
            CHECK(snx_ld_le32(mem + REPORT + 0x28) == 3 && mem[REPORT + 0x2c] == 0xee && mem[BUFFER + 3] == 0xee);
            /* an event id refused: the report left */
            strcpy((char*)mem + REPORT, "Upper");
            snx_st_le32(mem + REPORT + 0x28, 10);
            a.r[0] = REPORT;
            hook32_fn[k](&a);
            CHECK(a.r[0] == 0xa81 && snx_ld_le32(mem + REPORT + 0x28) == 10);
            /* a buffer with no room for the map: nothing written, nothing used */
            strcpy((char*)mem + REPORT, "event_1");
            snx_st_le32(mem + REPORT + 0x24, 2);
            mem[BUFFER] = 0xee;
            a.r[0] = REPORT;
            hook32_fn[k](&a);
            CHECK(a.r[0] == 0 && mem[BUFFER] == 0xee && snx_ld_le32(mem + REPORT + 0x28) == 0);
        }
        { /* the status of an empty queue in its 4 bytes */
            int k = hook_select(&hook32_table, all[3], versions, &first);
            memset(mem + 0x200, 0xee, 8);
            a.r[0] = 0x200, a.r[14] = 0x1234;
            hook32_fn[k](&a);
            CHECK(a.r[0] == 0 && snx_ld_le32(mem + 0x200) == 0 && mem[0x204] == 0xee);
        }
    }

    /* Send through prepo.hook, for the SDKs that can have it (15 on, as the session's SendReportWithUser): the context
       the caller passes in x8 comes back finished, with no object and success, its padding left; the report has no
       buffer after it; an older SDK's module gets no hook */
    {
        static const char send[] = PREPO_SYM_SEND;
        static const u32 with_send[] = {0x0f0000, 0x0f0302, 0x100200, 0x120302, 0x130303, 0x140503};
        u32 versions[HOOK_KEYS] = {0};
        int k = -1;
        versions[HOOK_SDK] = 0x0d0305;
        CHECK(hook_select(&hook_table, send, versions, &first) == -2);
        versions[HOOK_SDK] = 0x0e0300;
        CHECK(hook_select(&hook_table, send, versions, &first) == -2);
        for (unsigned i = 0; i < sizeof with_send / sizeof with_send[0]; i++) {
            versions[HOOK_SDK] = with_send[i];
            k = hook_select(&hook_table, send, versions, &first);
            CHECK(k >= 0 && !strcmp(hook_handler[k], "nnprepo_send"));
        }
        g_hle.prepo = (HlePrepo){0};
        for (int i = 0; i <= 901; i++) {
            const char* id = i == 901 ? "Bad id" : "event_1";
            report(&v20, id, 0x100);
            memset(mem + 0x300, 0xee, 0x10);
            c->x[0] = REPORT, c->x[1] = 0x200, c->x[8] = 0x300, c->x[30] = 0x1234, c->pc = 0;
            if (i == 901) test_ns += 5400ull * 1000000000ull; /* 90 minutes on: under the limit again */
            hook_fn[k](c, 0);
            if (i < 900) { /* sent: no object, success, no buffer left in the report */
                CHECK(c->pc == 0x1234 && snx_ld_le64(mem + 0x300) == 0 && snx_ld_le32(mem + 0x308) == 0);
                CHECK(snx_ld_le32(mem + 0x30c) == 0xeeeeeeeeu && hook_get_addr(c, v20.buffer, REPORT) == 0);
                CHECK(hook_get_u64(c, v20.capacity, REPORT) == 0x100 && !own_runs);
            } else if (i == 900) { /* past the limit: its Result in the context, the report as it was */
                CHECK(snx_ld_le64(mem + 0x300) == 0 && snx_ld_le32(mem + 0x308) == 0xc81);
                CHECK(hook_get_addr(c, v20.buffer, REPORT) == BUFFER && !own_runs);
            } else { /* an event id the SDK refuses: its own code, the context and the report untouched here */
                CHECK(own_runs == 1 && mem[0x300] == 0xee && hook_get_addr(c, v20.buffer, REPORT) == BUFFER);
            }
            test_ns += 1000000000ull;
        }
        CHECK(g_hle.prepo.saved == 0); /* Send's limit is its own, not Save's */
    }

    /* AddSessionId from SDK 9 (the session's GetSystemSessionId), as each SDK read has it: the SDK's own Add(key,
       Any64BitId) of the id 0, the 8 bytes below the caller's stack pointer with the stack pointer there during the
       call and back after it; none before */
    {
        static const char add_id[] = "_ZN2nn5prepo12AddSessionIdEPNS0_10PlayReportEPKc";
        static const u32 with_add_id[] = {0x090000, 0x090301, 0x0a0400, 0x0c0309, 0x0d0300,
                                          0x0e0300, 0x100200, 0x120302, 0x130303, 0x140506};
        static const u32 without[] = {0x080201, 0x08ffff};
        u32 versions[HOOK_KEYS] = {0};
        for (unsigned i = 0; i < sizeof without / sizeof without[0]; i++) {
            versions[HOOK_SDK] = without[i];
            CHECK(hook_select(&hook_table, add_id, versions, &first) == -2);
        }
        for (unsigned i = 0; i < sizeof with_add_id / sizeof with_add_id[0]; i++) {
            versions[HOOK_SDK] = with_add_id[i];
            int k = hook_select(&hook_table, add_id, versions, &first);
            CHECK(k >= 0 && !strcmp(hook_handler[k], "nnprepo_add_session_id"));
            memset(mem + 0x7c0, 0xee, 0x40);
            added.calls = 0;
            c->x[0] = REPORT, c->x[1] = 0x200, c->x[31] = 0x800, c->x[30] = 0x1234, c->pc = 0;
            hook_fn[k](c, 0);
            CHECK(added.calls == 1 && added.fn == ADD_AT && added.a[0] == REPORT && added.a[1] == 0x200);
            CHECK(added.a[2] == 0x7f0 && added.sp == 0x7f0 && added.id == 0 && mem[0x7f8] == 0xee &&
                  mem[0x7ef] == 0xee);
            CHECK(c->x[0] == 0x2a81 && c->x[31] == 0x800 && c->pc == 0x1234);
        }
    }

    /* the limit of SDK 9 on: the saves so far, then up to 900 in the 90 minutes; the 901st succeeds and leaves the report */
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
