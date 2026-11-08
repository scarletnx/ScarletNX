// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
/* nim:aoc, the eShop's add-on service: the commands that reach the shop answer 0xC8A0D027, as a console with no
   network does, so a game's add-on download menu shows its error instead of waiting for a shop that never answers;
   the local ones still answer success */
#include "3ds/runtime/internal.h"
#include "3ds/services/system.h"
#include "support/3ds_guest.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void call(SnxCtrProcess* p, uint32_t session, uint32_t header, SnxCtrIpc* c) {
    memset(c, 0, sizeof *c);
    c->process = p;
    c->object = snx_ctr_handle_object(p, session);
    c->in[0] = header;
    snx_ctr_system_dispatch(c);
}

int main(void) {
    uint8_t code[0x3000] = {0};
    SnxCtrImage image = {.segment = {{0x100000, 1, 16}, {0x200000, 1, 4}, {0x300000, 1, 4}},
                         .bss_size = 0x1000,
                         .stack_size = 0x4000,
                         .main_priority = 0x30,
                         .code = code,
                         .code_size = sizeof code};
    SnxCtrProcess* p = snx_ctr_process_new(&image, (SnxCtrModel)0);
    assert(p);
    uint32_t session = 0, opened = snx_ctr_system_open(p, (const uint8_t*)"nim:aoc", 7, &session);
    assert(!opened && session);
    SnxCtrIpc c;
    call(p, session, 0x00210000u, &c);
    assert(c.out[0] == 0x00210040u && c.out[1] == 0xC8A0D027u);
    const uint32_t shop[] = {0x000902D0u, 0x00180000u,
                             0x00250000u}; /* ListContentSetsEx, GetBalance, RefreshServerTime */
    for (unsigned i = 0; i < sizeof shop / sizeof *shop; ++i) {
        call(p, session, shop[i], &c);
        assert(c.out[0] == ((shop[i] & 0xFFFF0000u) | 0x40u) && c.out[1] == 0xC8A0D027u);
    }
    call(p, session, 0x00030042u, &c); /* SetApplicationId: local, still the unmodeled success */
    assert(c.out[0] == 0x00030040u && c.out[1] == 0);
    snx_ctr_process_free(p);
    puts("nim:aoc's eShop commands answer 0xC8A0D027, its local ones success");
    return 0;
}
