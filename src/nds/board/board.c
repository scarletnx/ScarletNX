/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The board's start and its time. A game starts as the firmware starts one from the card (melonDS's
   NDS::SetupDirectBoot gives the state: what main memory holds of the header and the card, CP15's registers, the
   stacks), with no firmware and no BIOS run. */
#include "nds/board/nds.h"
#include "nds/gpu/gpu3d.h"
#include "nds/hle/pxi.h"
#include "common/platform/time.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

bool nds_once(NdsBoard* board, unsigned key) {
    uint32_t* word = &board->told[key / 32 % 8];
    if (*word & 1u << key % 32) return false;
    *word |= 1u << key % 32;
    return true;
}

void nds_irq(NdsBoard* board, unsigned line) { board->irf |= 1u << line; }

void nds_schedule(NdsBoard* board, NdsEvent event, uint64_t at) {
    board->events[event] = at;
    uint64_t next = UINT64_MAX;
    for (unsigned i = 0; i < NDS_EVENT_COUNT; ++i)
        if (board->events[i] < next) next = board->events[i];
    board->next_event = next;
}

void nds_run_events(NdsBoard* board) {
    while (board->next_event <= board->cycles) {
        unsigned due = 0;
        for (unsigned i = 1; i < NDS_EVENT_COUNT; ++i)
            if (board->events[i] < board->events[due]) due = i;
        if (due == NDS_EVENT_LCD)
            nds_lcd_event(board);
        else if (due < NDS_EVENT_TIMER0 + 4)
            nds_timer_event(board, due - NDS_EVENT_TIMER0);
        else if (due == NDS_EVENT_SOUND)
            nds_pxi_tick(board);
        else
            nds_sound_alarm(board);
    }
}

/* The user's settings as the firmware copies them to 0x027FFC80 (GBATEK's "Firmware User Settings"): a name, a
   birthday, English, and a touch screen whose samples are its pixels times 16, as hle/pxi.c gives them */
static void user_settings(NdsBoard* board) {
    unsigned char* s = board->mem.base + 0x027FFC80u;
    static const char name[] = "ScarletNX";
    memset(s, 0, 0x70);
    snx_st_le16(s, 5);       /* version */
    s[2] = 2;                /* the favorite color */
    s[3] = 1, s[4] = 1;      /* the birthday: January 1 */
    for (unsigned i = 0; name[i]; ++i) snx_st_le16(s + 6 + i * 2, (uint8_t)name[i]);
    snx_st_le16(s + 0x1A, (uint16_t)(sizeof name - 1));
    snx_st_le16(s + 0x58, 0), snx_st_le16(s + 0x5A, 0); /* the first calibration point, raw then pixels */
    s[0x5C] = 0, s[0x5D] = 0;
    snx_st_le16(s + 0x5E, 255 << 4), snx_st_le16(s + 0x60, 191 << 4);
    s[0x62] = 255, s[0x63] = 191;
    snx_st_le16(s + 0x64, 1 | 3 << 4); /* English, the backlight at its brightest */
    s[0x66] = 26;                      /* the year, from 2000 */
}

bool nds_board_init(NdsBoard* board, NdsRom* rom, char* error, size_t error_size) {
    memset(board, 0, sizeof *board);
    board->rom = rom;
    if (!nds_memory_init(board, error, error_size)) return false;
    for (unsigned i = 0; i < NDS_EVENT_COUNT; ++i) board->events[i] = UINT64_MAX;
    board->next_event = UINT64_MAX;
    unsigned char* ram = board->mem.base;
    memcpy(ram + 0x027FFE00u, rom->data, NDS_HEADER_SIZE);
    snx_st_le32(ram + 0x027FF800u, rom->chip_id), snx_st_le32(ram + 0x027FF804u, rom->chip_id);
    snx_st_le16(ram + 0x027FF808u, rom->header_crc), snx_st_le16(ram + 0x027FF80Au, rom->secure_crc);
    snx_st_le16(ram + 0x027FF850u, 0x5835);
    snx_st_le32(ram + 0x027FFC00u, rom->chip_id), snx_st_le32(ram + 0x027FFC04u, rom->chip_id);
    snx_st_le16(ram + 0x027FFC08u, rom->header_crc), snx_st_le16(ram + 0x027FFC0Au, rom->secure_crc);
    snx_st_le16(ram + 0x027FFC10u, 0x5835);
    snx_st_le16(ram + 0x027FFC30u, 0xFFFF);
    snx_st_le16(ram + 0x027FFC40u, 0x0001);
    user_settings(board);
    if (rom->arm9_address - 0x02000000u + (uint64_t)rom->program_size > NDS_MAIN_RAM_SIZE) {
        snprintf(error, error_size, "the ARM9 program does not fit main memory: 0x%X bytes at 0x%08X",
                 rom->program_size, rom->arm9_address);
        return false;
    }
    memcpy(ram + rom->arm9_address, rom->program, rom->program_size);
    board->cp15_control = 0x00052078;
    board->cp15_dtcm = 0x0300000A;
    board->cp15_itcm = 0x00000020;
    nds_dtcm_remap(board);
    board->keyinput = 0x03FF;
    board->extkeyin = 0x007F;
    board->postflg = 1;
    board->powcnt = 0x820F;
    board->wramcnt = 3;
    board->exmemcnt = 0xE880;
    board->romctrl = 1u << 29;
    board->ipcfifocnt = 0;
    board->save_size = 0;
    nds_lcd_reset(board);
    if (!nds_gpu3d_init(board) || !nds_pxi_init(board)) {
        snprintf(error, error_size, "out of memory");
        return false;
    }
    return true;
}

void nds_board_free(NdsBoard* board) {
    nds_pxi_free(board);
    nds_gpu3d_free(board);
    nds_memory_free(board);
}
