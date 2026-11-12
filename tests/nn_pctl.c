/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native nn::pctl (src/nx/nn/pctl.c) on a stand-in CPU: the answers of a console with no parental controls set,
   and the play timer's display as nobody configured it, NotConfigured (7) in the first of its 0x18 bytes and the
   rest zero, which a game reads as no timer and not as TimesUp */
#include "nx/nn/pctl.c"
#include "hle/hook/hooks.hook.inc" /* pctl.hook's table, and its adapters for a 32-bit game */
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

static CPU cpu;
CPU* hook_a32_cpu(SnxArmCPU* arm) { return (void)arm, &cpu; }

int main(void) {
    static u8 mem[0x100];
    CPU* c = &cpu;
    cpu.mem = mem;
    CHECK(nnpctl_initialize(c) == 0);
    CHECK(nnpctl_permitted(c, 1) == 1 && nnpctl_permitted(c, 0) == 1);
    CHECK(nnpctl_yes(c) == 1 && nnpctl_success(c) == 0);
    CHECK(nnpctl_no(c) == 0 && nnpctl_none(c) == 0 && nnpctl_no_time(c) == 0);
    memset(mem, 0xee, sizeof mem);
    nnpctl_play_timer_display_info(c, 0x40);
    CHECK(mem[0x40] == 7);
    for (int i = 1; i < 0x18; i++) CHECK(mem[0x40 + i] == 0);
    CHECK(mem[0x3f] == 0xee && mem[0x58] == 0xee);
    /* a 32-bit game: every hook but the one that fills a struct has an adapter (pctl.hook's a32), and Initialize's
       answers success in r0 and goes back to lr; the play timer's 64-bit answer is r0 and r1 */
    {
        static SnxArmCPU a;
        int with = 0, without = 0;
        for (int i = 0; i < HOOK_COUNT; i++) {
            int info = strstr(hook_symbol[i], "DisplayInfo") != NULL;
            CHECK((hook_a32[i] == NULL) == info);
            with += hook_a32[i] != NULL, without += !hook_a32[i];
            if (!strcmp(hook_symbol[i], "_ZN2nn4pctl10InitializeEv")) {
                a.r[0] = 0xdead, a.r[14] = 0x2001;
                hook_a32[i](&a);
                CHECK(a.r[0] == 0 && a.r[15] == 0x2000 && (a.cpsr & SNX_ARM_T));
            }
            if (!strcmp(hook_symbol[i], "_ZN2nn4pctl25GetPlayTimerRemainingTimeEv")) {
                a.r[0] = a.r[1] = 0xdead, a.r[14] = 0x3000;
                hook_a32[i](&a);
                CHECK(a.r[0] == 0 && a.r[1] == 0 && a.r[15] == 0x3000);
            }
        }
        CHECK(with == 19 && without == 1);
    }
    printf("nn_pctl: %u checks passed\n", checks);
    return 0;
}
