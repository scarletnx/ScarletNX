/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2016-2022 melonDS team
   Copyright 2026 the ScarletNX contributors */
/* The ARM7's side of the inter-processor FIFO, answered without an ARM7: the SDK's program there is the same in every
   game but for its version, a server of requests the game's ARM9 side sends as words (a service in the low five bits,
   a flag, and 26 bits of data), each answered with a word back. Ported from the high-level ARM7 of melonDS
   (HLE_Retail/IPC.cpp of its melonHLE branch at b42e2f4, GPL-3.0-or-later): the handshake, the services' request and
   reply words, and what the ARM7 keeps in main memory for the game (the keys, the touch screen's sample, the clock).
   The devices behind the services are the board's own here: the firmware's settings, the host's clock, the window's
   pointer, the save memory's file. */
#include "nds/hle/pxi.h"
#include "nds/bios/bios.h"
#include "common/platform/time.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    PXI_FIRMWARE = 4,
    PXI_RTC = 5,
    PXI_TOUCH = 6,
    PXI_SOUND = 7,
    PXI_POWER = 8,
    PXI_MIC = 9,
    PXI_WIRELESS = 10,
    PXI_SAVE = 11,
    PXI_OS = 12,
    PXI_CARD = 13,
    PXI_15 = 15
};

typedef struct NdsPxi {
    uint16_t firmware[16], touch[16], power[16], mic[16]; /* a request's words as they come, sixteen at most */
    int touch_state;                                       /* 0 idle, 2 sampling by the line */
    unsigned touch_samples, touch_lines[4];
    uint32_t save_command, save_words, save_buffer;
    uint8_t power_register[8];
    /* the firmware's flash, 256 KiB: nothing but the header's fields a game reads and the user's settings, twice, at
       its end. A game's writes stay for the run */
    unsigned char* flash;
    uint8_t flash_write_enabled;
} NdsPxi;
#define FLASH_SIZE 0x40000u

static void reply(NdsBoard* board, uint32_t service, uint32_t data, uint32_t flag) {
    nds_ipc_reply(board, (service & 0x1F) | data << 6 | (flag & 1) << 5);
}

static bool in_main(uint32_t address) { return address >= 0x02000000u && address < 0x02800000u; }

/* the flash as the firmware's own would be for the settings the board made (board.c): GBATEK's "DS Firmware" */
static bool flash_make(NdsBoard* board) {
    NdsPxi* p = board->pxi;
    p->flash = malloc(FLASH_SIZE);
    if (!p->flash) return false;
    memset(p->flash, 0xFF, FLASH_SIZE);
    p->flash[0x1D] = 0xFF;                           /* the console's type: an NDS */
    snx_st_le16(p->flash + 0x20, 0x3FE00 >> 3);      /* where the user's settings are, in units of 8 bytes */
    for (unsigned copy = 0; copy < 2; ++copy) {
        unsigned char* settings = p->flash + 0x3FE00 + copy * 0x100;
        memset(settings, 0, 0x100);
        memcpy(settings, board->mem.base + 0x027FFC80u, 0x70);
        snx_st_le16(settings + 0x70, (uint16_t)copy); /* the update counter: the second copy is the newer */
        snx_st_le16(settings + 0x72, nds_crc16(0xFFFF, settings, 0x70));
    }
    return true;
}

static void firmware(NdsBoard* board, uint32_t data) {
    NdsPxi* p = board->pxi;
    if (data & 1u << 25) memset(p->firmware, 0, sizeof p->firmware);
    p->firmware[data >> 16 & 15] = (uint16_t)data;
    if (!(data & 1u << 24)) return;
    const uint16_t* w = p->firmware;
    uint32_t command = (uint32_t)(w[0] >> 8) - 0x20;
    switch (command) {
        case 0: /* write enable */
            p->flash_write_enabled = 1;
            reply(board, PXI_FIRMWARE, 0x0300A000, 0);
            break;
        case 1: /* write disable */
            p->flash_write_enabled = 0;
            reply(board, PXI_FIRMWARE, 0x0300A100, 0);
            break;
        case 2: { /* the status register: its write enable latch, never busy */
            uint32_t to = (uint32_t)(w[0] & 0xFF) << 24 | (uint32_t)w[1] << 8 | (w[2] >> 8 & 0xFF);
            if (!in_main(to)) {
                reply(board, PXI_FIRMWARE, 0x0300A202, 0);
                break;
            }
            nds_write8(board, to, (uint8_t)(p->flash_write_enabled << 1));
            reply(board, PXI_FIRMWARE, 0x0300A200, 0);
            break;
        }
        case 3: { /* read */
            uint32_t to = (uint32_t)w[4] << 16 | w[5], from = (uint32_t)(w[0] & 0xFF) << 16 | w[1];
            uint32_t length = (uint32_t)w[2] << 16 | w[3];
            if (!in_main(to) || length > 0x40000) {
                reply(board, PXI_FIRMWARE, 0x0300A302, 0);
                break;
            }
            for (uint32_t i = 0; i < length; ++i) nds_write8(board, to + i, p->flash[(from + i) & (FLASH_SIZE - 1)]);
            reply(board, PXI_FIRMWARE, 0x0300A300, 0);
            break;
        }
        case 5: { /* write: a page, kept for the run */
            uint32_t from = (uint32_t)w[3] << 16 | w[4], to = (uint32_t)(w[0] & 0xFF) << 16 | w[1], length = w[2];
            if (!in_main(from)) {
                reply(board, PXI_FIRMWARE, 0x0300A502, 0);
                break;
            }
            for (uint32_t i = 0; i < length; ++i) p->flash[(to + i) & (FLASH_SIZE - 1)] = nds_read8(board, from + i);
            p->flash_write_enabled = 0;
            reply(board, PXI_FIRMWARE, 0x0300A500, 0);
            break;
        }
        default:
            if (nds_once(board, 8)) fprintf(stderr, "scarletnx-nds: firmware request 0x%04X is not known\n", w[0]);
            break;
    }
}

static uint8_t bcd(int value) { return (uint8_t)(value / 10 << 4 | value % 10); }

/* the clock's date and time at 0x027FFDE8, as the RTC gives them: binary coded decimal, the afternoon's bit in the hour */
static void clock_read(NdsBoard* board, int date, int hours) {
    time_t now = (time_t)(plat_unix_time() + plat_tz_offset());
    struct tm t;
#ifdef _WIN32
    gmtime_s(&t, &now);
#else
    gmtime_r(&now, &t);
#endif
    unsigned char* out = board->mem.base + 0x027FFDE8u;
    if (date) {
        out[0] = bcd(t.tm_year % 100), out[1] = bcd(t.tm_mon + 1), out[2] = bcd(t.tm_mday);
        out[3] = (uint8_t)t.tm_wday;
    }
    if (hours) {
        out[4] = (uint8_t)(bcd(t.tm_hour) | (t.tm_hour >= 12 ? 0x40 : 0));
        out[5] = bcd(t.tm_min), out[6] = bcd(t.tm_sec);
    }
}

static void rtc(NdsBoard* board, uint32_t data) {
    uint32_t command = data >> 8 & 0x7F;
    if ((command >= 2 && command <= 15) || (command >= 26 && command <= 34) || command >= 42) {
        reply(board, PXI_RTC, 0x8001 | command << 8, 0);
        return;
    }
    switch (command) {
        case 0x10: clock_read(board, 1, 1), reply(board, PXI_RTC, 0x9000, 0); break;
        case 0x11: clock_read(board, 1, 0), reply(board, PXI_RTC, 0x9100, 0); break;
        case 0x12: clock_read(board, 0, 1), reply(board, PXI_RTC, 0x9200, 0); break;
        default:
            /* the alarms, the status registers and their writes: done, with nothing behind them */
            reply(board, PXI_RTC, 0x8000 | command << 8, 0);
            break;
    }
}

/* the touch screen's sample where the SDK reads it, 0x027FFFAA: 12 bits of x and of y, touched, and a validity */
static void touch_sample(NdsBoard* board) {
    unsigned char* at = board->mem.base + 0x027FFFAAu;
    uint32_t sample = snx_ld_le16(at) | (uint32_t)snx_ld_le16(at + 2) << 16;
    if (!board->touching) {
        sample &= 0xFE000000u;
        sample |= 0x06000000u;
    } else {
        sample &= 0xF9000000u;
        sample |= (uint32_t)(board->touch_x << 4 & 0xFFF) | (uint32_t)(board->touch_y << 4 & 0xFFF) << 12 | 0x01000000u;
    }
    snx_st_le16(at, (uint16_t)sample), snx_st_le16(at + 2, (uint16_t)(sample >> 16));
}

static void touch(NdsBoard* board, uint32_t data) {
    NdsPxi* p = board->pxi;
    if (data & 1u << 25) memset(p->touch, 0, sizeof p->touch);
    p->touch[data >> 16 & 15] = (uint16_t)data;
    if (!(data & 1u << 24)) return;
    switch (p->touch[0] >> 8) {
        case 0: /* one sample */
            touch_sample(board);
            reply(board, PXI_TOUCH, 0x03008000, 0);
            break;
        case 1: { /* samples by the line: how many a frame, and the first one's line */
            unsigned count = p->touch[0] & 0xFF, first = p->touch[1];
            if (p->touch_state) {
                reply(board, PXI_TOUCH, 0x03008103, 0);
                break;
            }
            if (!count || count > 4 || first >= NDS_FRAME_LINES) {
                reply(board, PXI_TOUCH, 0x03008102, 0);
                break;
            }
            p->touch_samples = count;
            for (unsigned i = 0; i < count; ++i) p->touch_lines[i] = (first + i * NDS_FRAME_LINES / count) % NDS_FRAME_LINES;
            p->touch_state = 2;
            reply(board, PXI_TOUCH, 0x03008100, 0);
            break;
        }
        case 2:
            if (p->touch_state != 2) {
                reply(board, PXI_TOUCH, 0x03008103, 0);
                break;
            }
            p->touch_samples = 0, p->touch_state = 0;
            reply(board, PXI_TOUCH, 0x03008200, 0);
            break;
        case 3:
            touch_sample(board);
            reply(board, PXI_TOUCH, 0x03008300, 0);
            break;
        default:
            if (nds_once(board, 9)) fprintf(stderr, "scarletnx-nds: touch screen request 0x%04X is not known\n", p->touch[0]);
            break;
    }
}

static void power(NdsBoard* board, uint32_t data) {
    NdsPxi* p = board->pxi;
    if (data & 1u << 25) memset(p->power, 0, sizeof p->power);
    p->power[data >> 16 & 15] = (uint16_t)data;
    if (!(data & 1u << 24)) return;
    uint32_t command = (uint32_t)(p->power[0] >> 8) - 0x60;
    uint8_t* r = p->power_register;
    switch (command) {
        case 3: /* the lights, the amplifier, the power: the power management chip's register 0 */
            switch (p->power[1] & 0xFF) {
                case 1: r[0] &= ~0x10; break;
                case 2: r[0] |= 0x30; break;
                case 3: r[0] = (uint8_t)((r[0] & ~0x20) | 0x10); break;
                case 4: r[0] |= 0x04; break;
                case 5: r[0] &= ~0x04; break;
                case 6: r[0] |= 0x08; break;
                case 7: r[0] &= ~0x08; break;
                case 8: r[0] |= 0x0C; break;
                case 9: r[0] &= ~0x0C; break;
                case 10: r[0] |= 0x01; break;
                case 11: r[0] &= ~0x01; break;
                case 12: r[0] |= 0x02; break;
                case 13: r[0] &= ~0x02; break;
                default: break;
            }
            reply(board, PXI_POWER, 0x0300E300, 0);
            break;
        case 4: /* a register's write */
            r[p->power[0] & 7] = (uint8_t)p->power[1];
            reply(board, PXI_POWER, 0x03008000 | ((p->power[1] + 0x70u) & 0xFF) << 8, 0);
            break;
        case 5: /* its read */
            reply(board, PXI_POWER, 0x03008000 | r[p->power[0] & 7] | ((p->power[1] + 0x70u) & 0xFF) << 8, 0);
            break;
        case 6: reply(board, PXI_POWER, 0x03008000 | ((p->power[1] + 0x70u) & 0xFF) << 8, 0); break;
        default:
            if (nds_once(board, 10))
                fprintf(stderr, "scarletnx-nds: power request 0x%04X 0x%04X is not known\n", p->power[0], p->power[1]);
            break;
    }
}

static void mic(NdsBoard* board, uint32_t data) {
    NdsPxi* p = board->pxi;
    if (data & 1u << 25) memset(p->mic, 0, sizeof p->mic);
    p->mic[data >> 16 & 15] = (uint16_t)data;
    if (!(data & 1u << 24)) return;
    uint32_t command = (uint32_t)(p->mic[0] >> 8) - 0x40;
    /* no microphone: each request is done, with silence where it samples */
    reply(board, PXI_MIC, 0x0300C000 | (command & 0x3F) << 8, 0);
}

#define SAVE_MOST 0x100000u
static bool save_room(NdsBoard* board, uint32_t end) {
    if (end > SAVE_MOST) return false;
    if (!board->save) {
        board->save = malloc(SAVE_MOST);
        if (!board->save) return false;
        memset(board->save, 0xFF, SAVE_MOST);
    }
    if (end > board->save_size) board->save_size = end;
    return true;
}

/* The card's save memory, which the SDK's ARM7 side reads and writes for the game: a first word names the request,
   request 0 gives the block the others take their arguments from (source, destination and length at 0x0C, 0x10 and
   0x14 of it, the result in its first word) */
static void save(NdsBoard* board, uint32_t data) {
    NdsPxi* p = board->pxi;
    if (!p->save_words) p->save_command = data;
    if (p->save_command == 0) {
        if (!p->save_words) {
            p->save_words = 1;
            return;
        }
        p->save_buffer = data;
        p->save_words = 0;
        reply(board, PXI_SAVE, 1, 1);
        return;
    }
    p->save_words = 0;
    uint32_t block = p->save_buffer, a = 0, b = 0, length = 0, result = 0;
    if (in_main(block)) {
        a = nds_read32(board, block + 0x0C), b = nds_read32(board, block + 0x10);
        length = nds_read32(board, block + 0x14);
    }
    switch (p->save_command) {
        case 6: /* read: from the save's a to memory's b */
            if (length <= SAVE_MOST && a <= SAVE_MOST - length && save_room(board, 0)) {
                for (uint32_t i = 0; i < length; ++i) nds_write8(board, b + i, a + i < SAVE_MOST ? board->save[a + i] : 0xFF);
            }
            break;
        case 7: /* write, program: from memory's a to the save's b */
        case 8:
            if (length <= SAVE_MOST && b <= SAVE_MOST - length && save_room(board, b + length)) {
                for (uint32_t i = 0; i < length; ++i) board->save[b + i] = nds_read8(board, a + i);
                board->save_dirty = 1;
            } else
                result = 4;
            break;
        case 9: /* verify */
            if (length <= SAVE_MOST && b <= SAVE_MOST - length && save_room(board, 0))
                for (uint32_t i = 0; i < length; ++i)
                    if (board->save[b + i] != nds_read8(board, a + i)) result = 1;
            break;
        default: break; /* what kind of memory it is, and its erases: done */
    }
    if (in_main(block)) nds_write32(board, block, result);
    reply(board, PXI_SAVE, 1, 1);
}

void nds_pxi_sync(NdsBoard* board) {
    unsigned value = board->ipcsync >> 8 & 15;
    /* the handshake the two sides start with: each count answered with the next, and the last with zero and the
       ARM7's services marked ready */
    if (value < 5)
        board->ipcsync = (uint16_t)((board->ipcsync & 0xFFF0) | (value + 1));
    else if (value == 5) {
        board->ipcsync &= 0xFFF0;
        snx_st_le32(board->mem.base + 0x027FFF8Cu, 0x0000FFF0);
    }
}

void nds_pxi_request(NdsBoard* board, uint32_t word) {
    uint32_t service = word & 0x1F, flag = word >> 5 & 1, data = word >> 6;
    if (board->trace) fprintf(stderr, "nds pxi: service %u flag %u data 0x%07X\n", service, flag, data);
    switch (service) {
        case PXI_FIRMWARE:
            if (!flag) firmware(board, data);
            break;
        case PXI_RTC:
            if (!flag) rtc(board, data);
            break;
        case PXI_TOUCH:
            if (!flag) touch(board, data);
            break;
        case PXI_SOUND: nds_sound_request(board, data); break;
        case PXI_POWER:
            if (!flag) power(board, data);
            break;
        case PXI_MIC:
            if (!flag) mic(board, data);
            break;
        case PXI_WIRELESS: break; /* no wireless: its requests get no answer, as with the radio off */
        case PXI_SAVE:
            if (flag) save(board, data);
            break;
        case PXI_OS:
            if (data == 0x1000) reply(board, PXI_OS, 0x1000, 0);
            break;
        case PXI_CARD:
            if ((data & 0x3F) == 1) reply(board, PXI_CARD, 1, 0);
            break;
        case PXI_15:
            if (data == 0x10000) reply(board, PXI_15, 0x10000, 0);
            break;
        default:
            if (nds_once(board, 11 + service % 16))
                fprintf(stderr, "scarletnx-nds: the game asks the ARM7's service %u (0x%08X), which is not known\n",
                        service, word);
            break;
    }
}

void nds_pxi_line(NdsBoard* board, unsigned line) {
    NdsPxi* p = board->pxi;
    /* the keys the ARM9 has no register for, where the ARM7 keeps them: X, Y, and the hinge */
    snx_st_le16(board->mem.base + 0x027FFFA8u,
                (uint16_t)((board->extkeyin & 1) << 10 | (board->extkeyin >> 1 & 1) << 11 | 1u << 13 |
                           (board->extkeyin >> 7 & 1) << 15));
    for (unsigned i = 0; i < p->touch_samples; ++i)
        if (line == p->touch_lines[i]) {
            touch_sample(board);
            reply(board, PXI_TOUCH, 0x03009000 | i, 0);
            break;
        }
    /* the frame counter the ARM7 keeps for the game, a frame at each vertical blank */
    if (line == NDS_SCREEN_HEIGHT) {
        unsigned char* count = board->mem.base + 0x027FFC3Cu;
        snx_st_le32(count, snx_ld_le32(count) + 1);
    }
}

void nds_pxi_tick(NdsBoard* board) {
    /* 174592 bus cycles, 5.2 ms: the timer the SDK's sound driver runs by */
    nds_schedule(board, NDS_EVENT_SOUND, board->events[NDS_EVENT_SOUND] + 174592);
    nds_sound_tick(board);
}

bool nds_pxi_init(NdsBoard* board) {
    board->pxi = calloc(1, sizeof *board->pxi);
    if (!board->pxi) return false;
    board->pxi->power_register[0] = 0x0D; /* the sound on, both backlights on */
    if (!flash_make(board) || !nds_sound_init(board)) return false;
    nds_schedule(board, NDS_EVENT_SOUND, board->cycles + 174592);
    return true;
}

void nds_pxi_free(NdsBoard* board) {
    nds_sound_free(board);
    if (board->pxi) free(board->pxi->flash);
    free(board->pxi);
    free(board->save);
    board->pxi = NULL, board->save = NULL;
}
