/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nds/bios/bios.h"
#include <stdio.h>

static uint32_t isqrt(uint32_t value) {
    uint32_t root = 0, bit = 1u << 30;
    while (bit > value) bit >>= 2;
    for (; bit; bit >>= 2) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else
            root >>= 1;
    }
    return root;
}

/* count units of 16 or 32 bits from source to destination, or the first one count times */
static void cpu_set(NdsBoard* board, uint32_t source, uint32_t destination, uint32_t control, int fast) {
    uint32_t count = control & 0x1FFFFF;
    int fill = control >> 24 & 1, wide = fast || (control >> 26 & 1);
    if (fast) count = (count + 7) & ~7u;
    if (wide) {
        source &= ~3u, destination &= ~3u;
        uint32_t value = fill ? nds_read32(board, source) : 0;
        for (uint32_t i = 0; i < count; ++i, destination += 4) {
            if (!fill) value = nds_read32(board, source), source += 4;
            nds_write32(board, destination, value);
        }
    } else {
        source &= ~1u, destination &= ~1u;
        uint16_t value = fill ? nds_read16(board, source) : 0;
        for (uint32_t i = 0; i < count; ++i, destination += 2) {
            if (!fill) value = nds_read16(board, source), source += 2;
            nds_write16(board, destination, value);
        }
    }
}

uint16_t nds_crc16(uint16_t crc, const unsigned char* bytes, uint32_t length) {
    for (uint32_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) crc = (uint16_t)(crc & 1 ? crc >> 1 ^ 0xA001 : crc >> 1);
    }
    return crc;
}

static void bit_unpack(NdsBoard* board, uint32_t source, uint32_t destination, uint32_t info) {
    uint32_t length = nds_read16(board, info), in_width = nds_read8(board, info + 2);
    uint32_t out_width = nds_read8(board, info + 3), offset = nds_read32(board, info + 4);
    int zeros = offset >> 31 != 0;
    offset &= 0x7FFFFFFFu;
    if (!in_width || in_width > 8 || !out_width || out_width > 32) return;
    uint32_t word = 0, filled = 0;
    for (uint32_t i = 0; i < length; ++i) {
        uint32_t byte = nds_read8(board, source + i);
        for (uint32_t bit = 0; bit < 8; bit += in_width) {
            uint32_t value = byte >> bit & ((1u << in_width) - 1);
            if (value || zeros) value += offset;
            word |= filled < 32 ? value << filled : 0;
            filled += out_width;
            if (filled >= 32) {
                nds_write32(board, destination, word);
                destination += 4, word = 0, filled = 0;
            }
        }
    }
}

/* A writer of decompressed bytes: one at a time, or two at a time as a halfword (the BIOS's "Vram" calls) */
typedef struct {
    NdsBoard* board;
    uint32_t at;
    int halfwords, held;
    uint8_t low;
} Out;
static void put(Out* out, uint8_t byte) {
    if (!out->halfwords) {
        nds_write8(out->board, out->at++, byte);
        return;
    }
    if (!out->held) {
        out->low = byte, out->held = 1;
        return;
    }
    nds_write16(out->board, out->at, (uint16_t)(out->low | byte << 8));
    out->at += 2, out->held = 0;
}

static void lz77(NdsBoard* board, uint32_t source, uint32_t destination, int halfwords) {
    uint32_t header = nds_read32(board, source), size = header >> 8, made = 0;
    Out out = {board, destination, halfwords, 0, 0};
    source += 4;
    while (made < size) {
        unsigned flags = nds_read8(board, source++);
        for (int bit = 0; bit < 8 && made < size; ++bit, flags <<= 1) {
            if (!(flags & 0x80)) {
                put(&out, nds_read8(board, source++));
                ++made;
                continue;
            }
            unsigned high = nds_read8(board, source), low = nds_read8(board, source + 1);
            source += 2;
            uint32_t length = (high >> 4) + 3, distance = (((high & 15u) << 8) | low) + 1;
            for (uint32_t i = 0; i < length && made < size; ++i, ++made) {
                /* a byte the writer still holds is the last one made */
                uint32_t from = destination + made - distance;
                uint8_t byte = halfwords && out.held && distance == 1 ? out.low : nds_read8(board, from);
                put(&out, byte);
            }
        }
    }
}

static void run_length(NdsBoard* board, uint32_t source, uint32_t destination, int halfwords) {
    uint32_t header = nds_read32(board, source), size = header >> 8, made = 0;
    Out out = {board, destination, halfwords, 0, 0};
    source += 4;
    while (made < size) {
        unsigned flag = nds_read8(board, source++);
        if (flag & 0x80) {
            uint8_t byte = nds_read8(board, source++);
            for (unsigned n = (flag & 0x7F) + 3; n && made < size; --n, ++made) put(&out, byte);
        } else
            for (unsigned n = (flag & 0x7F) + 1; n && made < size; --n, ++made) put(&out, nds_read8(board, source++));
    }
}

static void huffman(NdsBoard* board, uint32_t source, uint32_t destination) {
    uint32_t header = nds_read32(board, source), size = header >> 8, bits = header & 15, made = 0;
    if (bits != 4 && bits != 8) return;
    uint32_t tree = source + 4, stream = tree + (nds_read8(board, tree) + 1u) * 2, node = tree + 1;
    uint32_t word = 0, filled = 0;
    while (made < size) {
        uint32_t data = nds_read32(board, stream);
        stream += 4;
        for (int bit = 31; bit >= 0 && made < size; --bit) {
            unsigned entry = nds_read8(board, node), right = data >> bit & 1;
            uint32_t next = (node & ~1u) + (entry & 0x3F) * 2 + 2 + right;
            if (entry & (right ? 0x40 : 0x80)) {
                word |= (uint32_t)nds_read8(board, next) << filled;
                filled += bits;
                made += bits == 8;
                if (bits == 4 && !(filled & 7)) ++made;
                if (filled == 32 || made == size) {
                    nds_write32(board, destination, word);
                    destination += 4, word = 0, filled = 0;
                }
                node = tree + 1;
            } else
                node = next;
        }
    }
}

static void unfilter(NdsBoard* board, uint32_t source, uint32_t destination, unsigned width) {
    uint32_t size = nds_read32(board, source) >> 8;
    source += 4;
    if (width == 1) {
        uint8_t value = 0;
        for (uint32_t i = 0; i < size; ++i) {
            value = (uint8_t)(value + nds_read8(board, source + i));
            nds_write8(board, destination + i, value);
        }
    } else {
        uint16_t value = 0;
        for (uint32_t i = 0; i + 1 < size + 1 && i < size; i += 2) {
            value = (uint16_t)(value + nds_read16(board, source + i));
            nds_write16(board, destination + i, value);
        }
    }
}

int nds_bios_call(NdsBoard* board, uint32_t number, uint32_t r[16], uint64_t* cycles) {
    *cycles = 0;
    switch (number) {
        case NDS_SWI_WAIT_BY_LOOP:
            /* four cycles of the CPU's clock a turn, two of the bus's */
            *cycles = (uint64_t)r[0] * 2;
            return 0;
        case NDS_SWI_DIV: {
            int32_t n = (int32_t)r[0], d = (int32_t)r[1];
            if (!d) { /* the BIOS's loop never ends: the answer of a division that gave up */
                r[1] = r[0];
                r[0] = n < 0 ? 0xFFFFFFFFu : 1;
                r[3] = 1;
            } else if (n == INT32_MIN && d == -1) {
                r[0] = 0x80000000u, r[1] = 0, r[3] = 0x80000000u;
            } else {
                int32_t q = n / d;
                r[0] = (uint32_t)q, r[1] = (uint32_t)(n % d), r[3] = q < 0 ? 0u - (uint32_t)q : (uint32_t)q;
            }
            return 0;
        }
        case NDS_SWI_CPU_SET: cpu_set(board, r[0], r[1], r[2], 0); return 0;
        case NDS_SWI_CPU_FAST_SET: cpu_set(board, r[0], r[1], r[2], 1); return 0;
        case NDS_SWI_SQRT: r[0] = isqrt(r[0]); return 0;
        case NDS_SWI_GET_CRC16:
            /* over memory as the window has it: no register is summed */
            if (r[1] >> 24 != 0x04 && (uint64_t)r[1] + r[2] <= NDS_MAP_SIZE)
                r[0] = nds_crc16((uint16_t)r[0], board->mem.base + r[1], r[2]);
            return 0;
        case NDS_SWI_IS_DEBUGGER: r[0] = 0; return 0;
        case NDS_SWI_BIT_UNPACK: bit_unpack(board, r[0], r[1], r[2]); return 0;
        case NDS_SWI_LZ77_WRAM: lz77(board, r[0], r[1], 0); return 0;
        case NDS_SWI_RL_WRAM: run_length(board, r[0], r[1], 0); return 0;
        /* The calls that read through the game's own functions (r3 names them: open, close, a byte, a halfword, a
           word). Every one the SDK gives reads the memory at r0 on, so that memory is read here, and the functions
           are not called: a game that reads a file or the card through them gets what memory has there */
        case NDS_SWI_LZ77_CALLBACK:
            if (nds_once(board, 3))
                fprintf(stderr, "scarletnx-nds: the game decompresses through the BIOS with its own reader, taken as "
                                "one that reads memory\n");
            lz77(board, r[0], r[1], 1);
            r[0] = nds_read32(board, r[0]) >> 8;
            return 0;
        case NDS_SWI_RL_CALLBACK:
            run_length(board, r[0], r[1], 1);
            r[0] = nds_read32(board, r[0]) >> 8;
            return 0;
        case NDS_SWI_HUFF_CALLBACK:
            huffman(board, r[0], r[1]);
            r[0] = nds_read32(board, r[0]) >> 8;
            return 0;
        case NDS_SWI_DIFF8: unfilter(board, r[0], r[1], 1); return 0;
        case NDS_SWI_DIFF16: unfilter(board, r[0], r[1], 2); return 0;
        case NDS_SWI_CUSTOM_POST: board->postflg = (uint8_t)(r[0] & 3) | (board->postflg & 1); return 0;
        default: return -1;
    }
}
