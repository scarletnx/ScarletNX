/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/nvn_command_gap.h"

int nvn_command_gap_methods(const u32* words, u32 length, NvnGapMethodVisitor visit, void* context) {
    if (!words || !length || length > 16384) return 0;
    /* Preflight the entire gap before emitting any state. A truncated tail must not commit earlier binds. */
    for (u32 i = 0; i < length;) {
        u32 header = words[i], mode = header >> 29, count = (header >> 16) & 0x1fff;
        if (mode == 4) {
            i++;
            continue;
        }
        if ((mode != 1 && mode != 3 && mode != 5) || !count || count >= length - i) return 0;
        i += count + 1;
    }
    if (!visit) return 1;
    for (u32 i = 0; i < length;) {
        u32 header = words[i++], mode = header >> 29, sub = (header >> 13) & 7;
        u32 method = header & 0xfff, count = (header >> 16) & 0x1fff;
        if (mode == 4) {
            visit(sub, method, count, context);
            continue;
        }
        for (u32 k = 0; k < count; k++) {
            u32 actual_method = mode == 1 ? method + k : mode == 5 && k ? method + 1 : method;
            visit(sub, actual_method, words[i + k], context);
        }
        i += count;
    }
    return 1;
}

/* puller methods (below 0x40) act the same on every subchannel, as gpu.c decodes them; the rest are the 3D engine's */
static int method_no_write(u32 subchannel, u32 method, u32 value) {
    if (method < 0x40)
        return method == 0x2 || (method >= 0x4 && method <= 0x6) ||
               (method == 0x7 && ((value & 0x1f) == 1 || (value & 0x1f) == 4));
    return !subchannel && ((method >= 0x8e0 && method <= 0x8e3) ||
                           (method >= 0x904 && method <= 0x924 && ((method - 0x904) & 7) == 0) || method == 0x544 ||
                           method == 0x701 || method == 0x702 || method == 0x7c0 || method == 0x7c1 ||
                           /* Zero-valued tiled-cache flush and PIPE_NOP change no logical backing bytes.
                              Other values remain unclassified. */
                           ((method == 0x3e0 || method == 0x68b) && !value) || (method >= 0x6c0 && method <= 0x6c2) ||
                           (method == 0x6c3 && (value & 3) == 1));
}

static void check_no_write(u32 subchannel, u32 method, u32 value, void* context) {
    if (!method_no_write(subchannel, method, value)) *(int*)context = 0;
}

int nvn_command_gap_no_writes(const u32* words, u32 length) {
    int valid = 1;
    return nvn_command_gap_methods(words, length, check_no_write, &valid) && valid;
}

typedef struct {
    NvnGapMemoryState state;
    NvnGapMemoryWrite* writes;
    u32 count, capacity;
    u64 last_address;
    u32 last_size;
    u8 last_kind, invalid;
} MemoryScan;

static void emit_memory_write(MemoryScan* scan, u64 address, u32 size, u8 kind) {
    if (scan->invalid) return;
    if (kind == NVN_GAP_WRITE_CBUF && scan->count && scan->last_kind == kind &&
        scan->last_address <= UINT64_MAX - scan->last_size && scan->last_address + scan->last_size == address &&
        scan->last_size <= (16u << 20) - size) {
        scan->last_size += size;
        if (scan->writes) scan->writes[scan->count - 1].size = scan->last_size;
        return;
    }
    if (scan->count == scan->capacity) {
        scan->invalid = 1;
        return;
    }
    if (scan->writes) scan->writes[scan->count] = (NvnGapMemoryWrite){.address = address, .size = size, .kind = kind};
    scan->count++;
    scan->last_address = address;
    scan->last_size = size;
    scan->last_kind = kind;
}

static void scan_memory_write(u32 subchannel, u32 method, u32 value, void* context) {
    MemoryScan* scan = context;
    if (method == 0x4) {
        scan->state.puller_high = value;
        scan->state.puller_known |= 1;
        return;
    }
    if (method == 0x5) {
        scan->state.puller_low = value;
        scan->state.puller_known |= 2;
        return;
    }
    if (method == 0x6) {
        scan->state.puller_payload = value;
        scan->state.puller_known |= 4;
        return;
    }
    if (method == 0x7 && (value & 0x1f) == 2) {
        if ((scan->state.puller_known & 3) != 3) {
            scan->invalid = 1;
            return;
        }
        u64 address = ((u64)(scan->state.puller_high & 0xff) << 32) | scan->state.puller_low;
        emit_memory_write(scan, address, (value & (1u << 24)) ? 4 : 16, NVN_GAP_WRITE_PULLER);
        if (!scan->invalid && scan->writes) {
            scan->writes[scan->count - 1].value = scan->state.puller_payload;
            scan->writes[scan->count - 1].value_known = !!(scan->state.puller_known & 4);
        }
        return;
    }
    if (subchannel) return;
    if (!method) {
        scan->state.query_known = scan->state.cbuf_known = 0;
        return;
    }
    if (method >= 0xe00) {
        scan->state.query_known = scan->state.cbuf_known = 0;
        return;
    }
    switch (method) {
        case 0x6c0:
            scan->state.query_high = value;
            scan->state.query_known |= 1;
            return;
        case 0x6c1:
            scan->state.query_low = value;
            scan->state.query_known |= 2;
            return;
        case 0x6c2:
            scan->state.query_sequence = value;
            scan->state.query_known |= 4;
            return;
        case 0x8e1:
            scan->state.cbuf_high = value;
            scan->state.cbuf_known |= 1;
            return;
        case 0x8e2:
            scan->state.cbuf_low = value;
            scan->state.cbuf_known |= 2;
            return;
        case 0x8e3:
            scan->state.cbuf_offset = value;
            scan->state.cbuf_known |= 4;
            return;
        case 0x6c3:
            if ((value & 3) == 0 || (value & 3) == 2) {
                if ((scan->state.query_known & 3) != 3) {
                    scan->invalid = 1;
                    return;
                }
                u64 address = ((u64)(scan->state.query_high & 0xff) << 32) | scan->state.query_low;
                u32 size =
                    (value & 3) == 0 ? ((value & ((1u << 4) | (1u << 28))) ? 4 : 16) : ((value & (1u << 28)) ? 4 : 16);
                emit_memory_write(scan, address, size, NVN_GAP_WRITE_QUERY);
                if (!scan->invalid && scan->writes) {
                    NvnGapMemoryWrite* write = &scan->writes[scan->count - 1];
                    u32 selector = (value >> 23) & 31;
                    write->fake_known = 1;
                    write->fake_report = (value & 3) == 2 && selector != 0;
                    if ((value & 3) == 0) {
                        write->value = scan->state.query_sequence;
                        write->value_known = !!(scan->state.query_known & 4);
                    } else if (!selector) {
                        write->value = 0;
                        write->value_known = 1;
                    }
                }
            }
            return;
        default: break;
    }
    if (method >= 0x8e4 && method <= 0x8f3) {
        if (scan->state.cbuf_known != 7) {
            scan->invalid = 1;
            return;
        }
        if (scan->state.cbuf_offset > UINT32_MAX - 4) {
            scan->invalid = 1;
            return;
        }
        u64 base = ((u64)scan->state.cbuf_high << 32) | scan->state.cbuf_low;
        if (base > UINT64_MAX - scan->state.cbuf_offset) {
            scan->invalid = 1;
            return;
        }
        u64 address = base + scan->state.cbuf_offset;
        if (address > UINT64_MAX - 4) {
            scan->invalid = 1;
            return;
        }
        emit_memory_write(scan, address, 4, NVN_GAP_WRITE_CBUF);
        scan->state.cbuf_offset += 4;
    }
}

int nvn_command_gap_memory_writes(const u32* words, u32 count, NvnGapMemoryState* state, NvnGapMemoryWrite* writes,
                                  u32 capacity, u32* write_count) {
    if (!state || !write_count || (capacity && !writes)) return 0;
    MemoryScan scan = {.state = *state, .capacity = capacity};
    if (!nvn_command_gap_methods(words, count, scan_memory_write, &scan) || scan.invalid) return 0;
    scan.state = *state;
    scan.writes = writes;
    scan.count = 0;
    scan.last_address = scan.last_size = 0;
    scan.last_kind = 0;
    if (!nvn_command_gap_methods(words, count, scan_memory_write, &scan) || scan.invalid) return 0;
    *state = scan.state;
    *write_count = scan.count;
    return 1;
}

int nvn_command_gap_indexed_draw(const u32* words, u32 length, NvnGapIndexedDraw* out) {
    if (!words || !out || length < 6 || length > 16384) return 0;
    for (u32 i = 0; i < length;) {
        u32 header = words[i];
        u32 mode = header >> 29, sub = (header >> 13) & 7, method = header & 0xfff;
        u32 count = (header >> 16) & 0x1fff;
        if (mode == 4) {
            i++;
            continue;
        }
        if ((mode != 1 && mode != 3 && mode != 5) || !count || count >= length - i) return 0;
        if (mode == 5 && sub == 0 && method == 0xe38 && count == 5 && i + 6 == length) {
            u32 primitive = words[i + 1], high = words[i + 2], low = words[i + 3];
            u32 index_type = words[i + 4], vertices = words[i + 5];
            if (primitive > 6 || high >= 256 || index_type > 2 || !vertices || vertices > (1u << 24)) return 0;
            *out = (NvnGapIndexedDraw){((u64)high << 32) | low, primitive, index_type, vertices};
            return 1;
        }
        i += count + 1;
    }
    return 0;
}

int nvn_command_gap_modeled_indexed_draw(const u32* words, u32 count) {
    NvnGapIndexedDraw draw;
    if (!nvn_command_gap_indexed_draw(words, count, &draw)) return 0;
    /* The recognized final MME packet is one header plus five words. Every
       earlier packet must be provably read-only; a decoded write in the prefix
       still needs its own ordered scope and cannot be hidden by the draw. */
    return count == 6 || nvn_command_gap_no_writes(words, count - 6);
}

static void check_staged_write(u32 subchannel, u32 method, u32 value, void* context) {
    if (method_no_write(subchannel, method, value)) return;
    if ((method == 0x7 && (value & 0x1f) == 2) ||
        (!subchannel &&
         ((method == 0x6c3 && ((value & 3) == 0 || (value & 3) == 2)) || (method >= 0x8e4 && method <= 0x8f3))))
        return;
    *(int*)context = 0;
}

int nvn_command_gap_staged_writes(const u32* words, u32 count, int indexed_draw_staged) {
    if (!words || !count || count > 16384) return 0;
    u32 prefix = count;
    if (indexed_draw_staged) {
        NvnGapIndexedDraw draw;
        if (!nvn_command_gap_indexed_draw(words, count, &draw)) return 0;
        prefix -= 6;
    }
    if (!prefix) return 1;
    int valid = 1;
    return nvn_command_gap_methods(words, prefix, check_staged_write, &valid) && valid;
}

typedef struct {
    u32 macro_method;
    NvnGapMacroData3 packet;
    u8 phase, found, invalid;
} MacroData3Scan;

static void scan_macro_data3(u32 subchannel, u32 method, u32 value, void* context) {
    MacroData3Scan* scan = context;
    if (scan->invalid) return;
    if (!subchannel && method == scan->macro_method) {
        if (scan->phase || scan->found)
            scan->invalid = 1;
        else {
            scan->packet.macro_argument = value;
            scan->phase = 1;
        }
        return;
    }
    if (!scan->phase) return;
    if (subchannel || method != 0x8e3 + scan->phase) {
        scan->invalid = 1;
        return;
    }
    scan->packet.words[scan->phase - 1] = value;
    if (++scan->phase == 4) {
        scan->found = 1;
        scan->phase = 0;
    }
}

int nvn_command_gap_macro_data3(const u32* words, u32 count, u32 macro_method, NvnGapMacroData3* out) {
    if (!out || macro_method < 0xe00 || macro_method > 0xfff) return 0;
    MacroData3Scan scan = {.macro_method = macro_method};
    if (!nvn_command_gap_methods(words, count, scan_macro_data3, &scan) || !scan.found || scan.phase || scan.invalid)
        return 0;
    *out = scan.packet;
    return 1;
}
