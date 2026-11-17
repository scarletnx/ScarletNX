/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The os_light family's LightEvent handlers (src/nx/hle/hook/os_sync.c) on each layout hooks.hook gives
   nn::os::LightEventType: InitializeLightEvent leaves the bytes the SDK's own leaves, so the SDK's TimedWait, which
   the family leaves to the game's code, reads a waiter word of 0, 1 or 2. The bytes are those of the SDK's own
   InitializeLightEvent, read from games of 6.5.0 to 12.3.1 (the waiter word at 0, the automatic clear at 4,
   initialized at 5, the word at 8 cleared) and of 13.3.0 to 18.3.2 (the automatic clear at 0, initialized at 1, the
   waiter word at 4). An SDK before 13 given the later layout found 0x101 at its waiter word, and aborted. TryWait,
   Clear and a Signal with no waiter take the word as the SDK's do, with no wait in the kernel */
#include "nx/hle/hook/os_sync.c"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the runtime's side, which none of the calls below reaches: each stops the test if it is */
Hle g_hle;
int g_lock_futex;
[[noreturn]] static void unreached(const char* what) {
    fprintf(stderr, "os_light_event: %s was called\n", what);
    abort();
}
ClockRate clock_rate(void) { unreached("clock_rate"); }
void core_yield(KThread* t, int migrate) { (void)t, (void)migrate, unreached("core_yield"); }
void fatal(const char* fmt, ...) { unreached(fmt); }
u64 guest_ticks(void) { unreached("guest_ticks"); }
void hook_wait_begin(void) { unreached("hook_wait_begin"); }
void hook_wait_end(void) { unreached("hook_wait_end"); }
u64 kernel_find_symbol_exact(const char* name) { unreached(name); }
void kthread_block(KThread* t) { (void)t, unreached("kthread_block"); }
KThread* kthread_cur(void) { unreached("kthread_cur"); }
void kthread_unblock(KThread* t) { (void)t, unreached("kthread_unblock"); }
u32 svc_arbitrate_unlock(u64 addr) { (void)addr, unreached("svc_arbitrate_unlock"); }
u32 svc_signal_addr(u64 addr, u32 type, i32 value, i32 count) {
    (void)addr, (void)type, (void)value, (void)count, unreached("svc_signal_addr");
}
u32 svc_wait_addr(KThread* t, u64 addr, u32 type, i64 value, i64 timeout) {
    (void)t, (void)addr, (void)type, (void)value, (void)timeout, unreached("svc_wait_addr");
}

static u8 g_guest[0x100];

static u32 at32(u32 offset) { return snx_ld_le32(g_guest + offset); }

/* the bytes the SDK's InitializeLightEvent leaves, for signaled and mode, with junk under it first */
static void expect_init(CPU* c, LightEventLayout* L, int old, int signaled, u32 mode) {
    memset(g_guest, 0xcc, sizeof g_guest);
    if (old)
        os_light_event_init_old(c, L, 0x10, signaled, mode);
    else
        os_light_event_init(c, L, 0x10, signaled, mode);
    const u32 waiter = signaled ? LW_SIGNALED : 0u, automatic = mode == 1;
    if (old) {
        /* str xzr, [x0]; strb auto, [x0, #4]; str waiter, [x0]; str wzr, [x0, #8]; strb 1, [x0, #5] */
        assert(at32(0x10) == waiter && g_guest[0x14] == automatic && g_guest[0x15] == 1 && at32(0x18) == 0);
        assert(g_guest[0x16] == 0 && g_guest[0x17] == 0 && g_guest[0x1c] == 0xcc); /* str xzr cleared 6 and 7, nothing past */
    } else {
        /* strb 1, [x0, #1]; strb auto, [x0]; str waiter, [x0, #4] */
        assert(g_guest[0x10] == automatic && g_guest[0x11] == 1 && at32(0x14) == waiter);
        assert(g_guest[0x12] == 0xcc && g_guest[0x18] == 0xcc);
    }
}

static void layout(CPU* c, LightEventLayout* L, int old) {
    const u32 w = 0x10 + L->waiter.offset;
    for (int signaled = 0; signaled < 2; signaled++)
        for (u32 mode = 0; mode < 2; mode++) expect_init(c, L, old, signaled, mode);
    /* manual: a signal with nobody waiting sets the word, TryWait keeps it, Clear takes it */
    expect_init(c, L, old, 0, 0);
    assert(!os_light_event_try_wait(c, L, 0x10));
    os_light_event_signal(c, L, 0x10);
    assert(at32(w) == LW_SIGNALED && os_light_event_try_wait(c, L, 0x10) && at32(w) == LW_SIGNALED);
    os_light_event_clear(c, L, 0x10);
    assert(at32(w) == 0);
    /* automatic: TryWait takes the signal */
    expect_init(c, L, old, 1, 1);
    assert(os_light_event_try_wait(c, L, 0x10) && at32(w) == 0 && !os_light_event_try_wait(c, L, 0x10));
    /* a signaled one's Wait returns at once, an automatic one taking the signal */
    expect_init(c, L, old, 1, 1);
    os_light_event_wait(c, L, 0x10);
    assert(at32(w) == 0);
    expect_init(c, L, old, 1, 0);
    os_light_event_wait(c, L, 0x10);
    assert(at32(w) == LW_SIGNALED);
}

/* the variants hookgen made of hooks.hook (its table, static in the generated .inc, read as text): each one's size and
   offsets, and the comment that names its versions */
static int read_variants(HookLayout_nn_os_LightEventType out[2], char ranges[2][64]) {
    FILE* f = fopen(OS_LIGHT_INC, "r");
    char line[512];
    int n = -1;
    if (!f) return 0;
    while (fgets(line, sizeof line, f) && n < 2) {
        if (n < 0) {
            if (strstr(line, "hook_layout_nn_os_LightEventType[2] = {")) n = 0;
            continue;
        }
        unsigned size, waiter, automatic, initialized;
        if (sscanf(line, " {%x, {%x}, {%x}, {%x}}", &size, &waiter, &automatic, &initialized) != 4) break;
        out[n] = (HookLayout_nn_os_LightEventType){size, {waiter}, {automatic}, {initialized}};
        const char* sdk = strstr(line, "sdk ");
        snprintf(ranges[n], sizeof ranges[n], "%s", sdk ? sdk : "");
        n++;
    }
    fclose(f);
    return n;
}

int main(void) {
    CPU c = {0};
    c.mem = g_guest;
    HookLayout_nn_os_LightEventType v[2];
    char ranges[2][64];
    assert(read_variants(v, ranges) == 2);
    /* before 13 and from 13.3.0, as the SDK's own InitializeLightEvent of each lays it out */
    assert(strstr(ranges[0], "sdk >= 6.5.0, < 12.3.2") && strstr(ranges[1], "sdk >= 13.3.0"));
    assert(v[0].size == 0xc && v[0].waiter.offset == 0 && v[0].auto_clear.offset == 4 && v[0].initialized.offset == 5);
    assert(v[1].size == 8 && v[1].waiter.offset == 4 && v[1].auto_clear.offset == 0 && v[1].initialized.offset == 1);
    layout(&c, &v[0], 1);
    layout(&c, &v[1], 0);
    puts("LightEvent handlers: each SDK's layout, as its own InitializeLightEvent leaves it");
    return 0;
}
