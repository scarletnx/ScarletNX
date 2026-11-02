/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Which CLEAR_FLAGS bit cuts a clear to scissor 0: SCISSOR, bit 8, with the scissor enabled. */
#include "nx/gpu/gpu.h"
#include <stdio.h>

static u32 r[0x1000];
static int failures;
static void expect(int want, u32 flags, u32 scissor_on, const char* what) {
    r[0x43E] = flags;
    r[0x380] = scissor_on;
    if (r3d_clear_scissored(r) != want) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

int main(void) {
    expect(1, 0x100, 1, "SCISSOR with the scissor enabled cuts the clear");
    expect(0, 0x100, 0, "SCISSOR with the scissor disabled clears the target");
    expect(0, 0x010, 1, "CLEAR_RECT alone is not the scissor");
    expect(1, 0x111, 1, "SCISSOR among the other flags still cuts");
    expect(0, 0x1001, 1, "VIEWPORT and STENCIL_MASK are not the scissor");
    if (failures) return 1;
    puts("CLEAR_FLAGS: SCISSOR (bit 8) cuts a clear to an enabled scissor 0");
    return 0;
}
