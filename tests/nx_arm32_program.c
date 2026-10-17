/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A 32-bit Horizon program for nx_arm32_smoke.py: A32 code calling the SVCs whose 32-bit register layout differs from
   the 64-bit one, a Thumb thread, the coprocessor registers the SDK reads (TPIDRURO, CNTPCT), and indirect calls. It
   reports with svcOutputDebugString, which the runtime logs as "[game] ...", and ends with svcExitProcess. Built
   freestanding (-fropi: no relocations at run time), no game assets */
typedef unsigned int u32;
typedef unsigned long long u64;
#define NOINLINE __attribute__((noinline))
#define THUMB    __attribute__((target("thumb"), noinline))

static u32 length(const char* s) {
    u32 n = 0;
    while (s[n]) n++;
    return n;
}
NOINLINE static void print(const char* s) {
    register u32 r0 __asm__("r0") = (u32)s;
    register u32 r1 __asm__("r1") = length(s);
    __asm__ volatile("svc 0x27" : "+r"(r0), "+r"(r1) : : "memory");
}
NOINLINE static void print_hex(const char* label, u64 v) {
    char line[64];
    u32 n = 0, i;
    while (label[n] && n < 40) {
        line[n] = label[n];
        n++;
    }
    for (i = 0; i < 16; i++) line[n + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
    line[n + 16] = 0;
    print(line);
}
NOINLINE static void exit_process(void) {
    __asm__ volatile("svc 0x7" : : : "memory");
    for (;;) {}
}
NOINLINE static void fail(const char* what) {
    print(what);
    exit_process();
}

/* GetInfo: r1 type, r2 handle, the subtype in r0 (low) and r3 (high); the info in r1 (low) and r2 (high) */
NOINLINE static u32 get_info(u32 type, u32 handle, u64 subtype, u64* out) {
    register u32 r0 __asm__("r0") = (u32)subtype;
    register u32 r1 __asm__("r1") = type;
    register u32 r2 __asm__("r2") = handle;
    register u32 r3 __asm__("r3") = (u32)(subtype >> 32);
    __asm__ volatile("svc 0x29" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "memory");
    *out = r1 | (u64)r2 << 32;
    return r0;
}
NOINLINE static u32 set_heap_size(u32 size, u32* address) {
    register u32 r0 __asm__("r0") = 0;
    register u32 r1 __asm__("r1") = size;
    __asm__ volatile("svc 0x1" : "+r"(r0), "+r"(r1) : : "memory");
    *address = r1;
    return r0;
}
/* CreateThread: r1 entry, r2 argument, r3 stack top, r0 priority, r4 core */
NOINLINE static u32 create_thread(u32* handle, void (*entry)(u32*), u32* arg, u32 stack, int prio, int core) {
    register u32 r0 __asm__("r0") = (u32)prio;
    register u32 r1 __asm__("r1") = (u32)entry;
    register u32 r2 __asm__("r2") = (u32)arg;
    register u32 r3 __asm__("r3") = stack;
    register u32 r4 __asm__("r4") = (u32)core;
    __asm__ volatile("svc 0x8" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4) : : "memory");
    *handle = r1;
    return r0;
}
NOINLINE static u32 start_thread(u32 handle) {
    register u32 r0 __asm__("r0") = handle;
    __asm__ volatile("svc 0x9" : "+r"(r0) : : "r1", "r2", "r3", "memory");
    return r0;
}
/* WaitSynchronization: r1 handles, r2 count, the timeout in r0 (low) and r3 (high); the index in r1 */
NOINLINE static u32 wait_one(u32 handle, u64 timeout) {
    u32 handles[1] = {handle};
    register u32 r0 __asm__("r0") = (u32)timeout;
    register u32 r1 __asm__("r1") = (u32)handles;
    register u32 r2 __asm__("r2") = 1;
    register u32 r3 __asm__("r3") = (u32)(timeout >> 32);
    __asm__ volatile("svc 0x18" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "memory");
    return r0;
}
/* GetSystemTick: r0 (low), r1 (high) */
NOINLINE static u64 system_tick(void) {
    register u32 r0 __asm__("r0");
    register u32 r1 __asm__("r1");
    __asm__ volatile("svc 0x1e" : "=r"(r0), "=r"(r1) : : "memory");
    return r0 | (u64)r1 << 32;
}
/* SleepThread: the nanoseconds in r0 (low), r1 (high) */
NOINLINE static void sleep_ns(u64 ns) {
    register u32 r0 __asm__("r0") = (u32)ns;
    register u32 r1 __asm__("r1") = (u32)(ns >> 32);
    __asm__ volatile("svc 0xb" : "+r"(r0), "+r"(r1) : : "r2", "r3", "memory");
}

/* the Thumb thread: VFP arithmetic and a loop, its result through the argument, then svcExitThread */
THUMB static void worker(u32* result) {
    double x = 1.0;
    u32 i, sum = 0;
    for (i = 1; i <= 10; i++) {
        x = x * 1.5 + 0.25;
        sum += i * i;
    }
    result[0] = sum;
    result[1] = (u32)x;
    result[2] = 0xC0FFEE;
    __asm__ volatile("svc 0xa" : : : "memory");
}

/* reached through function pointers: twice is named by a relocation the module carries (the recompiler finds it), and
   thrice by nothing (the runtime compiles it when the program first calls it, and hints it) */
NOINLINE u32 nx_arm32_twice(u32 x) { return 2 * x; }
NOINLINE u32 nx_arm32_thrice(u32 x) { return 3 * x; }
typedef u32 (*Op)(u32);
NOINLINE static u32 apply(volatile Op* op, u32 x) { return (*op)(x); }

void nx_arm32_start(void) {
    u64 info, t0, t1, cnt0, cnt1;
    u32 heap, tls, handle, *shared, r;
    volatile Op op;
    print("A32: started");
    if (get_info(4, 0xFFFF8001u, 0, &info) || info != 0x80000000ull) fail("A32: GetInfo HeapRegionAddress is wrong");
    if (get_info(11, 0, 3, &info) || !info) fail("A32: GetInfo RandomEntropy 3 is wrong");
    print_hex("A32: heap region ", 0x80000000ull);
    if (set_heap_size(0x200000, &heap) || heap != 0x80000000u) fail("A32: SetHeapSize failed");
    shared = (u32*)heap;
    shared[0] = shared[1] = shared[2] = 0;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(tls));
    if (!tls || tls < 0xC0000000u) fail("A32: TPIDRURO is not the thread's TLS");
    print("A32: TLS ok");
    __asm__ volatile("mrrc p15, 0, %Q0, %R0, c14" : "=r"(cnt0));
    t0 = system_tick();
    sleep_ns(2000000ull);
    t1 = system_tick();
    __asm__ volatile("mrrc p15, 0, %Q0, %R0, c14" : "=r"(cnt1));
    if (t1 <= t0 || cnt1 <= cnt0 || cnt0 > t0 || t1 > cnt1) fail("A32: the tick counters don't advance together");
    print("A32: ticks ok");
    op = nx_arm32_twice;
    r = apply(&op, 21);
    op = nx_arm32_thrice;
    r += apply(&op, 7);
    if (r != 63) fail("A32: indirect calls are wrong");
    print("A32: indirect calls ok");
    if (create_thread(&handle, worker, shared, heap + 0x100000, 44, -2)) fail("A32: CreateThread failed");
    if (start_thread(handle)) fail("A32: StartThread failed");
    if (wait_one(handle, 5000000000ull)) fail("A32: WaitSynchronization on the thread failed");
    if (shared[0] != 385 || shared[1] != 85 || shared[2] != 0xC0FFEE) fail("A32: the Thumb thread's result is wrong");
    print("A32: Thumb thread ok");
    print("A32: done");
    exit_process();
}
