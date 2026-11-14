/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds_stub.h"
#include "common/utilities/endian.h"

void ctr_stub(unsigned char* text, uint32_t offset, uint32_t header_mov, uint32_t handle_global) {
    const uint32_t words[] = {
        0xe92d4030u, /* push {r4, r5, lr} */
        0xe1a05000u, /* mov r5, r0 */
        0xee1d4f70u, /* mrc p15, 0, r4, c13, c0, 3 */
        header_mov,  /* mov r0, #header */
        0xe5a40080u, /* str r0, [r4, #0x80]! */
        0xe59f001cu, /* ldr r0, [pc, #0x1c]: the handle's global */
        0xe5900000u, /* ldr r0, [r0] */
        0xef000032u, /* svc 0x32 */
        0xe2101102u, /* ands r1, r0, #0x80000000 */
        0x4a000002u, /* bmi to the pop */
        0xe5940008u, /* ldr r0, [r4, #8] */
        0xe5850000u, /* str r0, [r5] */
        0xe5940004u, /* ldr r0, [r4, #4] */
        0xe8bd8030u, /* pop {r4, r5, pc} */
        handle_global,
    };
    for (unsigned i = 0; i < sizeof words / sizeof *words; ++i) snx_st_le32(text + offset + 4 * i, words[i]);
}
