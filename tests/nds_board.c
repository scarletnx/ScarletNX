/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The NDS board without its CPU: the image's loader and its decompressor, the window's memories and the banks that
   move in it, the registers (the divider, the square root, the timers, DMA, the card), the BIOS's routines, the
   ARM7's answers over the FIFO, the firmware's Wi-Fi connections, and the geometry engine's commands. No game's
   data. */
#include "nds/audio/spu.h"
#include "nds/bios/bios.h"
#include "nds/board/nds.h"
#include "nds/cartridge/rom.h"
#include "nds/gpu/gpu3d.h"
#include "nds/gpu/gpu3d_state.h"
#include "nds/hle/pxi.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "support/nds_image.h"
#include "common/online/wfc.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                         \
    do {                                                                 \
        if (!(x)) {                                                      \
            fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #x); \
            exit(1);                                                     \
        }                                                                \
    } while (0)

#define IO 0x04000000u

static void test_blz(void) {
    /* "ABCDEFGH" five times: eight literals, then two copies from eight bytes on, read from the stream's end down */
    unsigned char data[40] = {0x05, 0xB0, 0x05, 0xF0, 0xC0, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 0x00,
                              22,   0,    0,    8,    18,   0,   0,   0};
    CHECK(nds_blz_size(data, 22) == 40);
    CHECK(nds_blz(data, 22, sizeof data) == 40);
    for (unsigned i = 0; i < 40; ++i) CHECK(data[i] == "ABCDEFGH"[i % 8]);
    unsigned char none[16] = {0};
    CHECK(nds_blz_size(none, 16) == 0 && nds_blz(none, 16, 16) == 0);
}

static void test_memory(NdsBoard* board) {
    unsigned char* w = board->mem.base;
    /* main memory four times, the shared work RAM and the ITCM through their ranges */
    nds_write32(board, 0x02000100, 0x11223344);
    CHECK(nds_read32(board, 0x02400100) == 0x11223344 && nds_read32(board, 0x02C00100) == 0x11223344);
    nds_write16(board, 0x01FF8002, 0xBEEF);
    CHECK(nds_read16(board, 0x00000002) == 0xBEEF && nds_read16(board, 0x01008002) == 0xBEEF);
    /* the DTCM where CP15 says: over main memory's last mirror, which shows again when it moves away */
    nds_write32(board, 0x027E0000, 0xAAAA5555);
    board->cp15_dtcm = 0x027E000A;
    nds_dtcm_remap(board);
    CHECK(board->mem.dtcm_base == 0x027E0000 && board->mem.dtcm_size == NDS_DTCM_SIZE);
    CHECK(nds_read32(board, 0x027E0000) != 0xAAAA5555);
    nds_write32(board, 0x027E0000, 0x12345678);
    CHECK(snx_ld_le32(board->mem.dtcm.view) == 0x12345678);
    CHECK(nds_read32(board, 0x023E0000) == 0xAAAA5555);
    board->cp15_dtcm = 0x0300000A;
    nds_dtcm_remap(board);
    CHECK(nds_read32(board, 0x027E0000) == 0xAAAA5555 && nds_read32(board, 0x03000000) == 0x12345678);
    /* a VRAM bank where VRAMCNT puts it: A as its own, then as a background's, and D as a texture's, which the ARM9
       does not see */
    nds_io_write(board, IO + 0x240, 1, 0x80);
    w[0x06800000] = 0x5A;
    CHECK(board->mem.vram[0].view[0] == 0x5A);
    nds_io_write(board, IO + 0x240, 1, 0x81 | 1 << 3);
    CHECK(w[0x06020000] == 0x5A && w[0x06800000] != 0x5A);
    nds_io_write(board, IO + 0x243, 1, 0x83);
    board->mem.vram[3].view[5] = 0x77;
    unsigned char* textures = malloc(nds_space_size[NDS_SPACE_TEXTURE]);
    CHECK(textures);
    nds_vram_copy(board, NDS_SPACE_TEXTURE, textures);
    CHECK(textures[5] == 0x77);
    free(textures);
}

static void test_registers(NdsBoard* board) {
    /* the divider: 64 by 32 bits, then by zero */
    nds_io_write(board, IO + 0x280, 2, 1);
    nds_io_write(board, IO + 0x290, 4, 1000000007u);
    nds_io_write(board, IO + 0x294, 4, 2);
    nds_io_write(board, IO + 0x298, 4, 1000);
    uint64_t n = (2ull << 32) + 1000000007u;
    CHECK(nds_io_read(board, IO + 0x2A0, 4) == (uint32_t)(n / 1000) && nds_io_read(board, IO + 0x2A8, 4) == n % 1000);
    CHECK(!(nds_io_read(board, IO + 0x280, 2) & 0x4000));
    nds_io_write(board, IO + 0x298, 4, 0);
    CHECK(nds_io_read(board, IO + 0x280, 2) & 0x4000);
    nds_io_write(board, IO + 0x2B0, 2, 0);
    nds_io_write(board, IO + 0x2B8, 4, 144);
    CHECK(nds_io_read(board, IO + 0x2B4, 4) == 12);
    /* a timer: 256 ticks of the bus clock to its overflow, which raises its interrupt and reloads it */
    board->irf = 0;
    nds_io_write(board, IO + 0x100, 2, 0xFF00);
    nds_io_write(board, IO + 0x102, 2, 0xC0);
    board->cycles += 0x80;
    CHECK(nds_io_read(board, IO + 0x100, 2) == 0xFF80);
    board->cycles += 0x80;
    nds_run_events(board);
    CHECK(board->irf & 1u << NDS_IRQ_TIMER0);
    CHECK(nds_io_read(board, IO + 0x100, 2) == 0xFF00);
    nds_io_write(board, IO + 0x102, 2, 0);
    /* DMA at once: 16 words, and its interrupt */
    for (unsigned i = 0; i < 16; ++i) nds_write32(board, 0x02010000 + i * 4, i * 0x01010101u);
    nds_io_write(board, IO + 0xB0, 4, 0x02010000);
    nds_io_write(board, IO + 0xB4, 4, 0x02020000);
    nds_io_write(board, IO + 0xB8, 4, 0x84000000u | 1u << 30 | 16);
    CHECK(nds_read32(board, 0x0202003C) == 15 * 0x01010101u);
    CHECK(!(nds_io_read(board, IO + 0xB8, 4) >> 31) && (board->irf & 1u << NDS_IRQ_DMA0));
    /* a channel is on the bus and does not reach the ITCM: a transfer to address 0 changes nothing there */
    nds_write32(board, 0x01FF8000, 0xE92D4000);
    nds_io_write(board, IO + 0xB4, 4, 0x00000000);
    nds_io_write(board, IO + 0xB8, 4, 0x84000000u | 16);
    CHECK(nds_read32(board, 0x01FF8000) == 0xE92D4000 && nds_read32(board, 0x00000000) == 0xE92D4000);
    /* the card's data read: 512 bytes from 0x8000 of the image, a word at a time */
    static const uint8_t command[8] = {0xB7, 0x00, 0x00, 0x80, 0x00, 0, 0, 0};
    for (unsigned i = 0; i < 8; ++i) nds_io_write(board, IO + 0x1A8 + i, 1, command[i]);
    nds_io_write(board, IO + 0x1A4, 4, 0x80000000u | 1u << 24);
    CHECK(nds_io_read(board, IO + 0x1A4, 4) & 1u << 23);
    CHECK(nds_io_read(board, IO + 0x100010, 4) == nds_test_program[0]);
    for (unsigned i = 1; i < 128; ++i) (void)nds_io_read(board, IO + 0x100010, 4);
    CHECK(!(nds_io_read(board, IO + 0x1A4, 4) >> 31));
    /* the same block through a DMA channel as the SDK sets it: the card's timing, repeated, a count of one word. The
       block is all there when the command is written, and the card's interrupt says it is done */
    nds_io_write(board, IO + 0x214, 4, 1u << NDS_IRQ_CARD_DONE);
    nds_io_write(board, IO + 0x1A0, 2, 0xC000);
    nds_io_write(board, IO + 0xD4, 4, 0x04100010);
    nds_io_write(board, IO + 0xD8, 4, 0x02040000);
    nds_io_write(board, IO + 0xDC, 4, 0xAF000001u);
    nds_io_write(board, IO + 0x1A4, 4, 0x80000000u | 1u << 24);
    CHECK(nds_read32(board, 0x02040000) == nds_test_program[0] && nds_read32(board, 0x02040004) == nds_test_program[1]);
    CHECK(!(nds_io_read(board, IO + 0x1A4, 4) >> 31) && (board->irf & 1u << NDS_IRQ_CARD_DONE));
    CHECK(nds_io_read(board, IO + 0xDC, 4) >> 31);
    nds_io_write(board, IO + 0xDC, 4, 0);
    nds_io_write(board, IO + 0x1A0, 2, 0);
    /* IF's bits go when a one is written to them */
    nds_io_write(board, IO + 0x214, 4, board->irf);
    CHECK(!(board->irf & ~(1u << NDS_IRQ_GXFIFO)));
}

static void test_bios(NdsBoard* board) {
    uint32_t r[16] = {0};
    uint64_t cycles;
    r[0] = (uint32_t)-100, r[1] = 7;
    CHECK(!nds_bios_call(board, NDS_SWI_DIV, r, &cycles));
    CHECK((int32_t)r[0] == -14 && (int32_t)r[1] == -2 && r[3] == 14);
    /* the smallest quotient: its size is 0x80000000 too, which a signed negation cannot give */
    r[0] = 0x80000000u, r[1] = 1;
    CHECK(!nds_bios_call(board, NDS_SWI_DIV, r, &cycles));
    CHECK(r[0] == 0x80000000u && r[1] == 0 && r[3] == 0x80000000u);
    r[0] = 1000000;
    CHECK(!nds_bios_call(board, NDS_SWI_SQRT, r, &cycles) && r[0] == 1000);
    /* the CRC of "123456789" from 0xFFFF: 0x4B37 */
    memcpy(board->mem.base + 0x02030000, "123456789", 9);
    r[0] = 0xFFFF, r[1] = 0x02030000, r[2] = 9;
    CHECK(!nds_bios_call(board, NDS_SWI_GET_CRC16, r, &cycles) && r[0] == 0x4B37);
    /* CpuSet: a fill of 8 words, then a copy of 4 halfwords */
    nds_write32(board, 0x02030100, 0xCAFEF00D);
    r[0] = 0x02030100, r[1] = 0x02030200, r[2] = 8 | 1u << 24 | 1u << 26;
    CHECK(!nds_bios_call(board, NDS_SWI_CPU_SET, r, &cycles));
    CHECK(nds_read32(board, 0x0203021C) == 0xCAFEF00D && nds_read32(board, 0x02030220) == 0);
    r[0] = 0x02030200, r[1] = 0x02030300, r[2] = 4;
    CHECK(!nds_bios_call(board, NDS_SWI_CPU_SET, r, &cycles));
    CHECK(nds_read32(board, 0x02030304) == 0xCAFEF00D && nds_read32(board, 0x02030308) == 0);
    /* LZ77: "ABCD", then eight bytes from four back */
    static const uint8_t lz[] = {0x10, 12, 0, 0, 0x08, 'A', 'B', 'C', 'D', 0x50, 0x03};
    memcpy(board->mem.base + 0x02030400, lz, sizeof lz);
    r[0] = 0x02030400, r[1] = 0x02030500;
    CHECK(!nds_bios_call(board, NDS_SWI_LZ77_WRAM, r, &cycles));
    CHECK(!memcmp(board->mem.base + 0x02030500, "ABCDABCDABCD", 12));
    /* run length: five of 'x', then "yz" */
    static const uint8_t rl[] = {0x30, 7, 0, 0, 0x82, 'x', 0x01, 'y', 'z'};
    memcpy(board->mem.base + 0x02030600, rl, sizeof rl);
    r[0] = 0x02030600, r[1] = 0x02030700;
    CHECK(!nds_bios_call(board, NDS_SWI_RL_WRAM, r, &cycles));
    CHECK(!memcmp(board->mem.base + 0x02030700, "xxxxxyz", 7));
    CHECK(nds_bios_call(board, 0x7F, r, &cycles) == -1);
}

static uint32_t fifo_read(NdsBoard* board) { return nds_io_read(board, IO + 0x100000, 4); }

static void test_arm7(NdsBoard* board) {
    /* the handshake: each count answered with the next, and the last with zero */
    for (unsigned count = 0; count < 5; ++count) {
        nds_io_write(board, IO + 0x180, 2, count << 8);
        CHECK((nds_io_read(board, IO + 0x180, 2) & 15) == count + 1);
    }
    nds_io_write(board, IO + 0x180, 2, 5 << 8);
    CHECK((nds_io_read(board, IO + 0x180, 2) & 15) == 0);
    CHECK(nds_read32(board, 0x027FFF8C) == 0x0000FFF0);
    nds_io_write(board, IO + 0x184, 2, 0x8000);
    CHECK(nds_io_read(board, IO + 0x184, 2) & 0x0100); /* nothing to receive */
    /* the clock: the date and time, in binary coded decimal */
    nds_io_write(board, IO + 0x188, 4, 5 | (0x10u << 8) << 6);
    CHECK(!(nds_io_read(board, IO + 0x184, 2) & 0x0100));
    CHECK(fifo_read(board) == (5 | 0x9000u << 6));
    const unsigned char* date = board->mem.base + 0x027FFDE8;
    CHECK((date[1] & 0x0F) <= 9 && date[1] >= 1 && date[1] <= 0x12 && date[2] >= 1 && date[2] <= 0x31);
    /* the touch screen: one sample, with the pen at (100, 50) */
    board->touching = 1, board->touch_x = 100, board->touch_y = 50;
    nds_io_write(board, IO + 0x188, 4, 6 | (1u << 25 | 1u << 24) << 6);
    CHECK(fifo_read(board) == (6 | 0x03008000u << 6));
    uint32_t sample = nds_read16(board, 0x027FFFAA) | (uint32_t)nds_read16(board, 0x027FFFAC) << 16;
    CHECK((sample & 0xFFF) == 100 << 4 && (sample >> 12 & 0xFFF) == 50 << 4 && (sample >> 24 & 1));
    /* the save memory: a block written, then read back from the save */
    nds_write32(board, 0x02040000 + 0x0C, 0x02040100); /* from memory */
    nds_write32(board, 0x02040000 + 0x10, 0x20);       /* to the save's 0x20 */
    nds_write32(board, 0x02040000 + 0x14, 8);
    memcpy(board->mem.base + 0x02040100, "savedata", 8);
    nds_io_write(board, IO + 0x188, 4, 11 | 1u << 5 | 0u << 6);
    nds_io_write(board, IO + 0x188, 4, 11 | 1u << 5 | 0x02040000u << 6);
    CHECK(fifo_read(board) == (11 | 1u << 5 | 1u << 6));
    nds_io_write(board, IO + 0x188, 4, 11 | 1u << 5 | 7u << 6);
    CHECK(fifo_read(board) == (11 | 1u << 5 | 1u << 6));
    CHECK(board->save_dirty && board->save_size == 0x28 && !memcmp(board->save + 0x20, "savedata", 8));
    nds_write32(board, 0x02040000 + 0x0C, 0x20);
    nds_write32(board, 0x02040000 + 0x10, 0x02040200);
    nds_io_write(board, IO + 0x188, 4, 11 | 1u << 5 | 6u << 6);
    CHECK(fifo_read(board) == (11 | 1u << 5 | 1u << 6));
    CHECK(!memcmp(board->mem.base + 0x02040200, "savedata", 8));
}

/* the firmware's bytes at from, length of them, read into main memory at to as a game reads them: the firmware
   service's words over the FIFO (service 4, each word's index in bits 16-19, the first marked by bit 25, the last by
   bit 24), command 3 (0x23 in the first word's high byte) */
static void firmware_read(NdsBoard* board, uint32_t from, uint32_t length, uint32_t to) {
    const uint16_t w[6] = {(uint16_t)(0x2300 | (from >> 16 & 0xFF)),
                           (uint16_t)from,
                           (uint16_t)(length >> 16),
                           (uint16_t)length,
                           (uint16_t)(to >> 16),
                           (uint16_t)to};
    for (uint32_t i = 0; i < 6; ++i)
        nds_io_write(board, IO + 0x188, 4,
                     4 | (i << 16 | w[i] | (i == 0 ? 1u << 25 : 0) | (i == 5 ? 1u << 24 : 0)) << 6);
    CHECK(fifo_read(board) == (4 | 0x0300A300u << 6));
}

/* the firmware's three Wi-Fi connections: none set, with their CRC, until the driver sets the first to a WFC
   service's DNS server, which a game then reads as it reads any of the firmware */
static void test_wfc(NdsBoard* board) {
    uint8_t* slot = board->mem.base + 0x02050000;
    SnxOnlineNdsWfcSlot read;
    char why[256] = "";
    firmware_read(board, 0x3FA00, 3 * SNX_ONLINE_NDS_WFC_SLOT_SIZE, 0x02050000);
    for (unsigned i = 0; i < 3; ++i) CHECK(snx_online_nds_wfc_slot_read(slot + i * 256, &read, why, sizeof why) == 0);
    static const uint8_t kaeru[4] = {178, 62, 43, 212};
    CHECK(nds_pxi_wfc(board, kaeru, why, sizeof why));
    firmware_read(board, 0x3FA00, 3 * SNX_ONLINE_NDS_WFC_SLOT_SIZE, 0x02050000);
    CHECK(snx_online_nds_wfc_slot_read(slot, &read, why, sizeof why) == 1);
    CHECK(!strcmp(read.ssid, NDS_WFC_NETWORK) && !memcmp(read.dns1, kaeru, 4) && !memcmp(read.dns2, kaeru, 4));
    CHECK(snx_online_nds_wfc_slot_read(slot + 256, &read, why, sizeof why) == 0);
    /* the user's settings after them are the board's as before */
    firmware_read(board, 0x3FE00, 0x74, 0x02050400);
    CHECK(snx_ld_le16(board->mem.base + 0x02050472) == nds_crc16(0xFFFF, board->mem.base + 0x02050400, 0x70));
}

/* the launcher's menu of WFC services (settings.txt's nds_wfc) is common/online's list: off (the default), auto, each
   service by its id and its name in the library's order, then custom */
static void test_wfc_menu(void) {
    const IniOpt* o = ini_find(g_nds_network_ini, "nds_wfc");
    CHECK(o && o->words && o->def);
    CHECK(!strcmp(o->def, "off"));
    char words[1024];
    snprintf(words, sizeof words, "%s", o->words);
    size_t count = 0;
    for (char* word = strtok(words, "|"); word; word = strtok(NULL, "|"), ++count) {
        char* eq = strchr(word, '=');
        CHECK(eq);
        *eq = 0;
        const SnxOnlineWfcPreset* p = count > 1 ? snx_online_wfc_preset_at(count - 2) : NULL;
        if (!count)
            CHECK(!strcmp(word, "off"));
        else if (count == 1)
            CHECK(!strcmp(word, "auto"));
        else if (p) {
            if (strcmp(word, p->id) || strcmp(eq + 1, p->name))
                fprintf(stderr, "nds_wfc's word %zu is %s=%s, common/online's service %s=%s\n", count, word, eq + 1,
                        p->id, p->name);
            CHECK(!strcmp(word, p->id) && !strcmp(eq + 1, p->name));
        } else
            CHECK(!strcmp(word, "custom"));
    }
    CHECK(count == snx_online_wfc_preset_count() + 3);
}

static void gx(NdsBoard* board, uint32_t command, uint32_t parameter) {
    nds_io_write(board, IO + 0x400 + (command << 2 & 0x1FC) + 0x40 - 0x40, 4, parameter);
}

static void test_geometry(NdsBoard* board) {
    const NdsGpu3D* g = nds_gpu3d_state(board);
    /* the identity for the projection and the position, the whole screen as the viewport, and a triangle */
    gx(board, 0x10, 0), gx(board, 0x15, 0);
    gx(board, 0x10, 2), gx(board, 0x15, 0);
    gx(board, 0x60, 0 | 0 << 8 | 255u << 16 | 191u << 24);
    gx(board, 0x29, 0x001F00C0); /* opaque, both faces */
    gx(board, 0x40, 0);
    gx(board, 0x20, 0x7FFF);
    gx(board, 0x25, (uint16_t)-0x800 | (uint32_t)(uint16_t)-0x800 << 16);
    gx(board, 0x25, 0x800 | (uint32_t)(uint16_t)-0x800 << 16);
    gx(board, 0x25, 0 | 0x800u << 16);
    gx(board, 0x41, 0);
    CHECK(nds_io_read(board, IO + 0x604, 4) == (1 | 3u << 16));
    /* the clip matrix reads back as the identity */
    CHECK(nds_io_read(board, IO + 0x640, 4) == 0x1000 && nds_io_read(board, IO + 0x644, 4) == 0);
    /* a box inside the view volume passes its test */
    nds_io_write(board, IO + 0x5C0, 4, 0), nds_io_write(board, IO + 0x5C0, 4, 0x01000000);
    nds_io_write(board, IO + 0x5C0, 4, 0x01000100);
    CHECK(nds_io_read(board, IO + 0x600, 4) & 2);
    /* the buffers swap at the vertical blank: until then the engine is busy and a command waits */
    gx(board, 0x50, 0);
    CHECK(nds_io_read(board, IO + 0x600, 4) & 1u << 27);
    gx(board, 0x10, 1);
    CHECK(g->waiting_count == 1 && g->matrix_mode == 2);
    nds_gpu3d_vblank(board);
    CHECK(!(nds_io_read(board, IO + 0x600, 4) & 1u << 27) && g->waiting_count == 0 && g->matrix_mode == 1);
    CHECK(g->render_num_polygons == 1 && nds_io_read(board, IO + 0x604, 4) == 0);
    const NdsPolygon* p = g->render_polygon_ram[0];
    CHECK(p->num_vertices == 3 && !p->translucent);
    CHECK(p->vertices[0]->final_position[0] == 64 && p->vertices[0]->final_position[1] == 144);
    CHECK(p->vertices[1]->final_position[0] == 192 && p->vertices[2]->final_position[1] == 48);
}

/* the matrix stack's sums of 20.12 products (multiplies and translate): a row and a column of the words
   -2^31, -2^31, 2^31 - 1 and 1 sum to 3 2^62 - 2^32 + 2, past int64_t, and the entry is that sum's bits 12 to 43 as
   the 64-bit wrap leaves them, 0xFFF00000 (and 0xFFF00000 again without the 1). In the texture matrix, which the
   clip matrix does not read */
static void load_column(NdsBoard* board, uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3) {
    for (uint32_t k = 0; k < 16; k++) gx(board, 0x16, k == 0 ? c0 : k == 4 ? c1 : k == 8 ? c2 : k == 12 ? c3 : 0);
}

static void test_matrices(NdsBoard* board) {
    const NdsGpu3D* g = nds_gpu3d_state(board);
    const uint32_t lo = 0x80000000u, hi = 0x7FFFFFFFu;
    gx(board, 0x10, 3);
    load_column(board, lo, lo, hi, 1);
    for (uint32_t k = 0; k < 16; k++) gx(board, 0x18, k == 0 || k == 1 ? lo : k == 2 ? hi : k == 3 ? 1 : 0);
    CHECK((uint32_t)g->tex_matrix[0] == 0xFFF00000u);
    load_column(board, lo, lo, hi, 0);
    for (uint32_t k = 0; k < 12; k++) gx(board, 0x19, k == 0 || k == 1 ? lo : k == 2 ? hi : 0);
    CHECK((uint32_t)g->tex_matrix[0] == 0xFFF00000u);
    load_column(board, lo, lo, hi, 0);
    for (uint32_t k = 0; k < 9; k++) gx(board, 0x1A, k == 0 || k == 1 ? lo : k == 2 ? hi : 0);
    CHECK((uint32_t)g->tex_matrix[0] == 0xFFF00000u);
    load_column(board, lo, lo, hi, 5);
    gx(board, 0x1C, lo), gx(board, 0x1C, lo), gx(board, 0x1C, hi);
    CHECK((uint32_t)g->tex_matrix[12] == 0xFFF00005u);
}

/* a vector through the vector matrix keeps the 32 bits of its sum: a matrix word of 2^30 + 1.0 times a component of
   511 passes int's range, and what the engine keeps of the product (bits 12 to 22 for a normal, 9 to 24 for the
   vector test) is 1.0's, so a lit normal's colour and the test's answer are the identity matrix's */
static void test_vectors(NdsBoard* board) {
    const NdsGpu3D* g = nds_gpu3d_state(board);
    uint8_t lit[3];
    gx(board, 0x10, 2), gx(board, 0x15, 0);
    gx(board, 0x30, 0x7FFF), gx(board, 0x31, 0);                         /* white diffuse, nothing else */
    gx(board, 0x32, 0x200), gx(board, 0x33, 0x7FFF);                     /* light 0, white, shining down -x */
    gx(board, 0x29, 0x001F00C1), gx(board, 0x40, 0), gx(board, 0x41, 0); /* light 0 on */
    gx(board, 0x21, 0x1FF);                                              /* a normal along +x, towards the light */
    memcpy(lit, g->vertex_color, sizeof lit);
    CHECK(lit[0] == 29 && lit[1] == 29 && lit[2] == 29); /* 31 * 31 * 511 >> 14 */
    gx(board, 0x72, 0x1FF);
    CHECK(nds_io_read(board, IO + 0x630, 4) == 0x0FF8 && nds_io_read(board, IO + 0x634, 2) == 0);
    for (uint32_t k = 0; k < 16; k++) gx(board, 0x16, k == 0 ? 0x40001000u : k % 5 == 0 ? 0x1000 : 0);
    gx(board, 0x21, 0x1FF);
    CHECK(!memcmp(lit, g->vertex_color, sizeof lit));
    gx(board, 0x72, 0x1FF);
    CHECK(nds_io_read(board, IO + 0x630, 4) == 0x0FF8 && nds_io_read(board, IO + 0x634, 2) == 0);
    gx(board, 0x15, 0), gx(board, 0x29, 0x001F00C0), gx(board, 0x40, 0), gx(board, 0x41, 0);
}

/* a box whose corners the clip matrix puts at x = z = 0x60000000 with w = -0x60000000, and the reverse: w - z passes
   int's range where an edge between them meets the far side of the view volume (kept, by the polygon's bit 12), and
   keeps its 32 bits. The sanitizers' build stops on the overflow; the answer is the wrapped one's */
static void test_clip(NdsBoard* board) {
    gx(board, 0x10, 0);
    for (uint32_t k = 0; k < 16; k++) gx(board, 0x16, k == 0 || k == 2 ? 0x60000000u : k == 3 ? 0xA0000000u : 0);
    gx(board, 0x10, 2), gx(board, 0x15, 0);
    gx(board, 0x29, 0x001F10C0), gx(board, 0x40, 0), gx(board, 0x41, 0);
    nds_io_write(board, IO + 0x5C0, 4, 0xF000);        /* x = -1.0, y = 0 */
    nds_io_write(board, IO + 0x5C0, 4, 0x2000u << 16); /* z = 0, width 2.0 */
    nds_io_write(board, IO + 0x5C0, 4, 0);             /* no height, no depth */
    uint32_t answer = nds_io_read(board, IO + 0x600, 4) & 2;
    nds_io_write(board, IO + 0x5C0, 4, 0xF000), nds_io_write(board, IO + 0x5C0, 4, 0x2000u << 16);
    nds_io_write(board, IO + 0x5C0, 4, 0);
    CHECK((nds_io_read(board, IO + 0x600, 4) & 2) == answer); /* and the same again */
    gx(board, 0x10, 0), gx(board, 0x15, 0), gx(board, 0x10, 2);
    gx(board, 0x29, 0x001F00C0), gx(board, 0x40, 0), gx(board, 0x41, 0);
}

/* a PSG channel's low half is -0x7FFF, which its volume's divider multiplies and never shifts: the sanitizers' build
   stops on a negative value shifted left. Half the samples of a 50% square are below zero */
static void test_spu(NdsBoard* board) {
    int16_t out[64 * 2];
    uint32_t below = 0, n;
    nds_spu_write(board, 0x04000500, 2, 0x807F);                                      /* the mixer on, full volume */
    nds_spu_write(board, 0x04000488, 2, 0x10000 - 0x800);                             /* channel 8's timer */
    nds_spu_write(board, 0x04000480, 4, 0xE0000000u | 3u << 24 | 0x40u << 16 | 0x7F); /* PSG, duty 3, centre, on */
    board->cycles += 64 * 1024;
    nds_spu_run(board);
    n = nds_spu_take(board, out, 64);
    for (uint32_t i = 0; i < n; ++i) below += out[i * 2] < 0;
    CHECK(n >= 32 && below && below < n);
    nds_spu_write(board, 0x04000480, 4, 0);
}

/* the sound driver's volumes are bytes a game sets whole, and its table of them holds 128 levels: a sequence at
   volume 255 that is stopped, which reads its tracks' volumes, reads inside the table. Nothing of the read shows
   without a note playing: it is the sanitizers' build that stops on one past the table */
static void test_sound(NdsBoard* board) {
    const uint32_t list = 0x02050000, notes = 0x02050100;
    const uint32_t commands[3][6] = {{list + 0x18, 2, 0, notes, 0, 0},     /* prepare sequence 0 over empty notes */
                                     {list + 0x30, 6, 0, 0x05, 255, 0x01}, /* its volume (key 0x105), 255 */
                                     {0, 1, 0, 0, 0, 0}};                  /* stop it */
    for (uint32_t i = 0; i < 3; ++i)
        for (uint32_t k = 0; k < 6; ++k) nds_write32(board, list + i * 0x18 + k * 4, commands[i][k]);
    nds_sound_request(board, list);
    nds_sound_request(board, 0);
}

/* SDK 2.0's first builds number the driver's commands their own way, through a table of 30: a command numbered past
   it names none, and reads nothing past the table (the sanitizers' build stops on that read) */
static void test_sound_early(NdsRom* rom) {
    char error[512] = "";
    const uint32_t list = 0x02050000;
    const uint32_t commands[2][6] = {{list + 0x18, 30, 0, 0, 0, 0}, {0, 0xFFFFFFFFu, 0, 0, 0, 0}};
    uint32_t sdk = rom->sdk_version;
    rom->sdk_version = 0x02001000;
    NdsBoard* board = calloc(1, sizeof *board);
    CHECK(board && nds_board_init(board, rom, error, sizeof error));
    for (uint32_t i = 0; i < 2; ++i)
        for (uint32_t k = 0; k < 6; ++k) nds_write32(board, list + i * 0x18 + k * 4, commands[i][k]);
    nds_sound_request(board, list);
    nds_sound_request(board, 0);
    nds_board_free(board);
    free(board);
    rom->sdk_version = sdk;
}

int main(void) {
    char error[512] = "";
    char* dir = snx_temp_dir(NULL);
    CHECK(dir);
    char* image = snx_path_join(dir, "test.nds");
    CHECK(image && nds_test_image(image, nds_test_program, nds_test_program_words));
    NdsRom rom;
    if (!nds_rom_open(&rom, image, NULL, error, sizeof error)) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    NdsBoard* board = calloc(1, sizeof *board);
    CHECK(board && nds_board_init(board, &rom, error, sizeof error));
    CHECK(nds_read32(board, NDS_TEST_ADDRESS) == nds_test_program[0]);
    CHECK(!memcmp(board->mem.base + 0x027FFE00, "SNXTEST", 7)); /* the header where the firmware leaves it */
    test_blz();
    test_memory(board);
    test_registers(board);
    test_bios(board);
    test_arm7(board);
    test_wfc(board);
    test_wfc_menu();
    test_geometry(board);
    test_matrices(board);
    test_vectors(board);
    test_clip(board);
    test_spu(board);
    test_sound(board);
    nds_board_free(board);
    free(board);
    test_sound_early(&rom);
    nds_rom_close(&rom);
    (void)snx_remove_tree(dir);
    free(image);
    free(dir);
    puts("nds_board: ok");
    return 0;
}
