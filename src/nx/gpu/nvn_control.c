/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/nvn_control.h"
#include <string.h>

static int packet_header(u32 word, u32 mode, u32 method, u32 count) {
    return (word >> 29) == mode && ((word >> 13) & 7) == 0 && (word & 0xfff) == method &&
           ((word >> 16) & 0x1fff) == count;
}

int nvn_control_macro38_draw(const u32* words, u32 count, NvnControlMacro38Draw* out) {
    if (!words || !out || count != 12 || !packet_header(words[0], 4, 0xe20, (words[0] >> 16) & 0x1fff) ||
        !packet_header(words[1], 4, 0x5f6, (words[1] >> 16) & 0x1fff) || !packet_header(words[2], 1, 0x5f2, 2) ||
        !packet_header(words[5], 5, 0xe4c, 6))
        return 0;
    u32 type = (words[1] >> 16) & 0x1fff;
    if (type > 2 || words[3] >= 256 || !words[7] || !words[8] || words[6] > 9) return 0;
    NvnControlMacro38Draw draw = {.index_address = ((u64)words[3] << 32) | words[4],
                                  .index_type = type,
                                  .primitive = words[6],
                                  .count = words[7],
                                  .instances = words[8],
                                  .base_instance = words[9],
                                  .base_vertex = words[10]};
    memcpy(draw.macro_args, &words[6], sizeof draw.macro_args);
    *out = draw;
    return 1;
}

int nvn_control_program_binds(const u32* words, u32 count, NvnControlProgramBind* out, u32 capacity, u32* bind_count) {
    if (!words || !bind_count || !count || count > (1u << 18) || (capacity && !out)) return 0;
    u32 found = 0;
    for (u32 i = 0; i < count;) {
        u32 header = words[i], mode = header >> 29, sub = (header >> 13) & 7;
        u32 method = header & 0xfff, n = (header >> 16) & 0x1fff;
        if (mode != 1 && mode != 3 && mode != 5) {
            i++;
            continue;
        }
        if (n >= count - i && n) return 0;
        if (!sub && method == 0xe18 && n == 5) found++;
        i += n + 1;
    }
    if (found > capacity) return 0;
    u32 used = 0;
    for (u32 i = 0; i < count;) {
        u32 header = words[i], mode = header >> 29;
        if (mode != 1 && mode != 3 && mode != 5) {
            i++;
            continue;
        }
        u32 n = (header >> 16) & 0x1fff;
        if (!((header >> 13) & 7) && (header & 0xfff) == 0xe18 && n == 5) {
            NvnControlProgramBind* bind = &out[used++];
            bind->selector = words[i + 1];
            bind->code_offset = words[i + 3];
            memcpy(bind->macro_args, words + i + 1, sizeof bind->macro_args);
        }
        i += n + 1;
    }
    *bind_count = used;
    return 1;
}
