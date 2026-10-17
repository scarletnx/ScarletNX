/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/nvn_control.h"
#undef NDEBUG /* the checks are the test */
#include <assert.h>

int main(void) {
    const u32 packet[] = {0x81900e20, 0x800205f6, 0x200205f2, 6, 0x56bd6ce8, 0xa0060e4c, 4, 0x5af3, 1, 0, 0x13703, 0};
    NvnControlMacro38Draw draw;
    assert(nvn_control_macro38_draw(packet, 12, &draw));
    assert(draw.index_address == 0x656bd6ce8ull);
    assert(draw.index_type == 2 && draw.primitive == 4 && draw.count == 0x5af3 && draw.instances == 1);
    assert(draw.base_instance == 0 && draw.base_vertex == 0x13703);
    assert(draw.macro_args[4] == 0x13703);
    u32 malformed[12];
    for (u32 i = 0; i < 12; i++) malformed[i] = packet[i];
    malformed[5] = 0xa0050e4c; /* truncated macro parameter count */
    assert(!nvn_control_macro38_draw(malformed, 12, &draw));
    assert(!nvn_control_macro38_draw(packet, 11, &draw));
    const u32 programs[] = {0x80000800, 0xa0050e18, 1,          0xa88,      0x00ca7f30, 0,          0x0400ca84,
                            0x80140813, 0x80510850, 0xa0050e18, 5,          0xa88,      0x01950b30, 4,
                            0x04019514, 0x80200853, 0x800003d0, 0x80000084, 0x800003c7, 0x20020e46, 0x087f6080,
                            0x00018020, 0x80000489, 0x8000065b, 0x800101d5, 0x20020820, 0x20,       0x30,
                            0x80000d1e, 0x80300830, 0x80400840, 0x8000047c};
    NvnControlProgramBind binds[2];
    u32 bind_count = 0;
    assert(nvn_control_program_binds(programs, 32, binds, 2, &bind_count));
    assert(bind_count == 2);
    assert(binds[0].selector == 1 && binds[0].code_offset == 0x00ca7f30);
    assert(binds[1].selector == 5 && binds[1].code_offset == 0x01950b30);
    assert(!nvn_control_program_binds(programs, 32, binds, 1, &bind_count));
    assert(!nvn_control_program_binds(programs, 27, binds, 2, &bind_count));
    const u32 no_data_mode[] = {0, 0xa0050e18, 1, 0xa88, 0x12330, 0, 0x40123};
    assert(nvn_control_program_binds(no_data_mode, 7, binds, 2, &bind_count));
    assert(bind_count == 1 && binds[0].code_offset == 0x12330);
    const u32 nonincrementing[] = {0x60050e18, 5, 0xa88, 0x45630, 4, 0x40456};
    assert(nvn_control_program_binds(nonincrementing, 6, binds, 2, &bind_count));
    assert(bind_count == 1 && binds[0].selector == 5 && binds[0].code_offset == 0x45630);
    return 0;
}
