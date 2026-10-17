/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* --script's presses against a clock and a present count the test sets: one at a time in ms is held press_len ms from
   then, one at present N ("Nf") from the first ask at or after that present, however late it comes, and both mix */
#define plat_ns test_ns
#include "nx/runtime/platform/unattended.c"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static u64 g_now;
u64 test_ns(void) { return g_now; }
int g_frames;
void snx_save_before_exit(void) {}
void ini_poll(int final) { (void) final; }
void plat_exit_now(int code) { exit(code); }
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static unsigned checks, failures;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            printf("FAIL line %d: ", __LINE__);                                                                        \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* the buttons at ms after the start, with frames presented */
static u64 at(u64 ms, int frames) {
    g_now = 1000000000ull + ms * 1000000ull;
    g_frames = frames;
    return unattended_buttons();
}

int main(void) {
    g_now = 1000000000ull;
    unattended_start();
    unattended_ini("script", "1000:1,5f:2,20f:400");
    CHECK(at(0, 0) == 0, "nothing at the start");
    CHECK(at(999, 4) == 0, "nothing before its time or present");
    CHECK(at(1000, 4) == 1, "A at 1000 ms: %llx", (unsigned long long)at(1000, 4));
    /* present 5 comes at 2000 ms: B is held from then, not from when the script was read */
    CHECK(at(2000, 5) == 2, "B from present 5: %llx", (unsigned long long)at(2000, 5));
    unattended_ini("script", "1000:1,5f:2,20f:400");
    CHECK(at(2299, 9) == 2, "B held 300 ms: %llx", (unsigned long long)at(2299, 9));
    CHECK(at(2300, 9) == 0, "B released after 300 ms: %llx", (unsigned long long)at(2300, 9));
    /* a present skipped over (frames go by two at a time) still presses */
    CHECK(at(5000, 21) == 0x400, "Plus once past present 20: %llx", (unsigned long long)at(5000, 21));
    CHECK(at(9000, 400) == 0, "each press once: %llx", (unsigned long long)at(9000, 400));
    CHECK(unattended_ms() == 9000, "the clock --script counts from: %llu", (unsigned long long)unattended_ms());
    unattended_ini("script", "1000:1,5f:2,20f:400");
    CHECK(at(9001, 401) == 0, "reloading unchanged script does not replay completed presses");
    unattended_ini("script", "402f:1");
    CHECK(at(9010, 402) == 1, "changed script starts its new press");
    CHECK(at(9310, 403) == 0, "new press ends normally");

    printf("%u checks, %u failed\n", checks, failures);
    return failures != 0;
}
