/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native nn::pctl (src/nx/nn/pctl.c) on a stand-in CPU: the answers of a console with no parental controls set,
   and the play timer's display as nobody configured it, NotConfigured (7) in the first of its 0x18 bytes and the
   rest zero, which a game reads as no timer and not as TimesUp */
#include "nx/nn/pctl.c"
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

int main(void) {
    static CPU cpu;
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
    printf("nn_pctl: %u checks passed\n", checks);
    return 0;
}
