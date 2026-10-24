/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
#include "agb/board/board.h"
#include "agb/ppu/ppu.h"
#include <stdio.h>
#include <string.h>
#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);                                           \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

/* The framebuffer holds 0xAABBGGRR, each palette channel's five bits widened to eight. */
#define WIDEN(v)     (((uint32_t)(v) << 3) | ((uint32_t)(v) >> 2))
#define PIX(r, g, b) (0xFF000000u | (WIDEN(b) << 16) | (WIDEN(g) << 8) | WIDEN(r))

static AgbBoard board;

static void run_frame(void) { agb_ppu_step(&board, AGB_SCANLINE_CYCLES * AGB_FRAME_LINES); }
static uint32_t pixel(int x, int y) { return board.ppu.framebuffer[y * AGB_SCREEN_WIDTH + x]; }

int main(void) {
    agb_board_init(&board);

    /* timing: VCOUNT walks the visible lines into the blank, and DISPSTAT reports both */
    agb_ppu_write(&board, 0x004, 0x0008 | (160 << 8)); /* vblank interrupt on, match line 160 */
    agb_ppu_step(&board, 960);                         /* the first line's visible dots are over */
    CHECK(agb_ppu_read(&board, 0x006) == 0);
    CHECK(agb_ppu_read(&board, 0x004) & 2); /* the horizontal blank runs to the end of the line */
    agb_ppu_step(&board, AGB_SCANLINE_CYCLES * 159 + 272);
    CHECK(agb_ppu_read(&board, 0x006) == 160);
    CHECK(agb_ppu_read(&board, 0x004) & 1); /* the vertical blank */
    CHECK(agb_ppu_read(&board, 0x004) & 4); /* the line matches the target */
    CHECK(agb_read16(&board, 0x04000202) & AGB_IRQ_VBLANK);
    run_frame();
    CHECK(agb_ppu_read(&board, 0x006) == 160); /* a frame of lines from here wraps back */
    agb_ppu_step(&board, AGB_SCANLINE_CYCLES * 68);
    CHECK(agb_ppu_read(&board, 0x006) == 0);
    agb_io_write16(&board, 0x04000202, 0xFFFF, 0xFFFF);

    /* mode 3: the bitmap is the screen */
    agb_ppu_write(&board, 0x000, 0x0403); /* mode 3, BG2 on */
    agb_write16(&board, 0x06000000, 0x001F);
    agb_write16(&board, 0x06000000 + (239 + 159 * 240) * 2, 0x03E0);
    run_frame();
    CHECK(pixel(0, 0) == PIX(31, 0, 0));
    CHECK(pixel(239, 159) == PIX(0, 31, 0));
    CHECK(pixel(5, 5) == PIX(0, 0, 0));

    /* mode 4: a paletted bitmap, and DISPCNT picks the page */
    agb_write16(&board, 0x05000002, 0x7C00); /* palette 1: blue */
    agb_write16(&board, 0x05000004, 0x03E0); /* palette 2: green */
    agb_write16(&board, 0x06000000 + 10 + 20 * 240, 0x0201);
    agb_write16(&board, 0x0600A000 + 30 + 40 * 240, 1);
    agb_ppu_write(&board, 0x000, 0x0404);
    run_frame();
    CHECK(pixel(10, 20) == PIX(0, 0, 31));
    CHECK(pixel(11, 20) == PIX(0, 31, 0));
    CHECK(pixel(12, 20) == PIX(0, 0, 0));
    agb_ppu_write(&board, 0x000, 0x0414);
    run_frame();
    CHECK(pixel(30, 40) == PIX(0, 0, 31));
    CHECK(pixel(10, 20) == PIX(0, 0, 0));

    /* mode 0: one text background, every screen entry the same tile in palette bank 1 */
    memset(board.memory.vram, 0, AGB_VRAM_SIZE);
    memset(board.memory.palette, 0, AGB_PALETTE_SIZE);
    agb_write16(&board, 0x05000022, 0x001F); /* bank 1, entry 1: red */
    agb_write16(&board, 0x05000024, 0x03E0); /* bank 1, entry 2: green */
    board.memory.vram[0] = 0x11;             /* tile 0, row 0: pixels 0 1 2 3 are 1 1 2 1 */
    board.memory.vram[1] = 0x12;
    for (int i = 0; i < 1024; ++i) agb_write16(&board, 0x0600F800 + (uint32_t)i * 2, 0x1000);
    agb_ppu_write(&board, 0x008, 0x1F00); /* BG0: char base 0, screen base 31 */
    agb_ppu_write(&board, 0x000, 0x0100); /* mode 0, BG0 on */
    run_frame();
    CHECK(pixel(0, 0) == PIX(31, 0, 0));
    CHECK(pixel(2, 0) == PIX(0, 31, 0));
    CHECK(pixel(3, 0) == PIX(31, 0, 0));
    CHECK(pixel(4, 0) == PIX(0, 0, 0)); /* the tile's second half is transparent */
    CHECK(pixel(0, 8) == PIX(31, 0, 0));
    agb_ppu_write(&board, 0x010, 0x1FC); /* BG0HOFS: -4 */
    run_frame();
    CHECK(pixel(0, 0) == PIX(0, 0, 0));
    CHECK(pixel(4, 0) == PIX(31, 0, 0));
    agb_ppu_write(&board, 0x010, 0);

    /* OBJ: an 8x8 sprite over the background */
    agb_write16(&board, 0x05000202, 0x7C00); /* OBJ palette 1: blue */
    board.memory.vram[0x10000] = 0x01;       /* tile 0, row 0: only pixel 0 is set */
    agb_write16(&board, 0x07000000, 0x0000); /* attr0: y = 0, normal */
    agb_write16(&board, 0x07000002, 10);     /* attr1: x = 10 */
    agb_write16(&board, 0x07000004, 0x0000); /* attr2: tile 0, priority 0 */
    agb_ppu_write(&board, 0x000, 0x1100);    /* mode 0, BG0 and OBJ on */
    run_frame();
    CHECK(pixel(10, 0) == PIX(0, 0, 31)); /* the sprite's pixel */
    CHECK(pixel(9, 0) == PIX(31, 0, 0));  /* the background beside it */
    CHECK(pixel(10, 1) == PIX(0, 0, 0));  /* the next sprite row is transparent */
    /* priority 3 puts the sprite under BG0, which is priority 0 */
    agb_write16(&board, 0x07000004, 3 << 10);
    run_frame();
    CHECK(pixel(10, 0) == PIX(0, 31, 0)); /* the background tile's third pixel */
    agb_write16(&board, 0x07000004, 0);
    /* attr0's bit 9 hides a sprite that is not affine (an affine one it draws at twice its size) */
    agb_write16(&board, 0x07000000, 0x0200);
    run_frame();
    CHECK(pixel(10, 0) == PIX(0, 31, 0)); /* the background, the sprite hidden */
    agb_write16(&board, 0x07000000, 0x0000);
    /* two sprites on one pixel: the lower OAM index over the higher at one priority, a lower priority value over a
       higher whatever the index, as on the hardware (a character drawn over its shadow) */
    agb_write16(&board, 0x05000222, 0x001F); /* OBJ palette bank 1, entry 1: red */
    agb_write16(&board, 0x07000008, 0x0000); /* sprite 1: y = 0 */
    agb_write16(&board, 0x0700000A, 10);     /* x = 10, over sprite 0 */
    agb_write16(&board, 0x0700000C, 0x1000); /* tile 0, priority 0, palette bank 1 */
    run_frame();
    CHECK(pixel(10, 0) == PIX(0, 0, 31));     /* sprite 0's blue over sprite 1's red */
    agb_write16(&board, 0x07000004, 1 << 10); /* sprite 0 at priority 1 */
    run_frame();
    CHECK(pixel(10, 0) == PIX(31, 0, 0)); /* sprite 1, priority 0, over it */
    agb_write16(&board, 0x07000004, 0);
    agb_write16(&board, 0x07000008, 0x0200); /* sprite 1 hidden */
    agb_ppu_write(&board, 0x000, 0x0100);    /* OBJ off */
    run_frame();
    CHECK(pixel(10, 0) == PIX(0, 31, 0));

    /* WIN0 keeps only the layers its control selects */
    agb_ppu_write(&board, 0x000, 0x3100);        /* mode 0, BG0 and OBJ, WIN0 on */
    agb_ppu_write(&board, 0x040, (8 << 8) | 11); /* WIN0H: x 8 to 11 */
    agb_ppu_write(&board, 0x044, 4);             /* WIN0V: y 0 to 4 */
    agb_ppu_write(&board, 0x048, 0x0001);        /* inside WIN0: only BG0 */
    agb_ppu_write(&board, 0x04A, 0x0011);        /* outside: BG0 and OBJ */
    board.memory.vram[0x10000] = 0x11;           /* the sprite covers x 10 and 11 */
    run_frame();
    CHECK(pixel(10, 0) == PIX(0, 31, 0)); /* inside WIN0 the sprite is off */
    CHECK(pixel(11, 0) == PIX(0, 0, 31)); /* outside it draws */
    agb_ppu_write(&board, 0x000, 0x1100);
    agb_ppu_write(&board, 0x040, 0);
    agb_ppu_write(&board, 0x048, 0);
    agb_ppu_write(&board, 0x04A, 0);

    /* brightness: the first target lightens toward white */
    agb_ppu_write(&board, 0x050, 0x0010 | (2 << 6)); /* OBJ first target, brightness up */
    agb_ppu_write(&board, 0x054, 8);
    run_frame();
    /* blue (0,0,31) rises by (31-0)*8/16 in red and green */
    CHECK(pixel(10, 0) == PIX(15, 15, 31));
    CHECK(pixel(11, 0) == PIX(15, 15, 31));
    agb_ppu_write(&board, 0x050, 0);
    agb_ppu_write(&board, 0x054, 0);

    /* a semi transparent sprite alpha blends over its second target */
    agb_write16(&board, 0x07000000, 0x0400); /* attr0: mode 1, semi transparent */
    board.memory.vram[0x10000] = 0x01;       /* only x 10 of the sprite is set */
    agb_write16(&board, 0x05000024, 0x001F); /* the background pixel under it goes red */
    agb_ppu_write(&board, 0x050, 0x0100);    /* BG0 is the second target */
    agb_ppu_write(&board, 0x052, 8 | (8 << 8));
    run_frame();
    /* blue over red at eva = evb = 8: (0,0,15) and (15,0,0) */
    CHECK(pixel(10, 0) == PIX(15, 0, 15));
    agb_ppu_write(&board, 0x050, 0);
    agb_ppu_write(&board, 0x052, 0);

    /* mosaic: a block of a layer that has it on shows its first pixel, and a block of lines its first line, as mGBA
       draws them. A 4bpp tile whose row 0 is red, green green green, then blue, and row 1 all blue */
    memset(board.memory.vram, 0, AGB_VRAM_SIZE);
    memset(board.memory.palette, 0, AGB_PALETTE_SIZE);
    memset(board.memory.oam, 0, AGB_OAM_SIZE);
    agb_write16(&board, 0x05000002, 0x001F); /* entry 1: red */
    agb_write16(&board, 0x05000004, 0x03E0); /* entry 2: green */
    agb_write16(&board, 0x05000006, 0x7C00); /* entry 3: blue */
    agb_write16(&board, 0x06000000, 0x2221); /* pixels 0 to 3: 1 2 2 2 */
    agb_write16(&board, 0x06000002, 0x3333);
    agb_write16(&board, 0x06000004, 0x3333); /* row 1 */
    agb_write16(&board, 0x06000006, 0x3333);
    agb_ppu_write(&board, 0x000, 0x0100); /* mode 0, BG0 on */
    agb_ppu_write(&board, 0x008, 0x1F00); /* BG0: char base 0, screen base 31, every entry tile 0 */
    agb_ppu_write(&board, 0x04C, 0x0003); /* BG blocks 4 pixels wide */
    run_frame();
    CHECK(pixel(1, 0) == PIX(0, 31, 0)); /* MOSAIC alone changes nothing: BG0CNT's bit 6 is off */
    agb_ppu_write(&board, 0x008, 0x1F40);
    run_frame();
    CHECK(pixel(1, 0) == PIX(31, 0, 0)); /* the block's first pixel, red */
    CHECK(pixel(3, 0) == PIX(31, 0, 0));
    CHECK(pixel(4, 0) == PIX(0, 0, 31));  /* the next block starts on blue */
    agb_ppu_write(&board, 0x04C, 0x0010); /* blocks 2 lines high */
    run_frame();
    CHECK(pixel(1, 1) == PIX(0, 31, 0)); /* line 1 shows row 0 */
    CHECK(pixel(1, 2) == PIX(0, 0, 0));  /* line 2 starts a block on row 2, which is empty */
    agb_ppu_write(&board, 0x008, 0);

    /* an affine background: BG2 in mode 1, the same pixels as 8bpp, screen base 30, no rotation */
    memset(board.memory.vram, 0, AGB_VRAM_SIZE);
    agb_write16(&board, 0x06000000, 0x0201);
    agb_write16(&board, 0x06000002, 0x0202);
    agb_write16(&board, 0x06000004, 0x0303);
    agb_write16(&board, 0x06000006, 0x0303);
    for (int i = 8; i < 16; i += 2) agb_write16(&board, 0x06000000 + (uint32_t)i, 0x0303); /* row 1 */
    agb_ppu_write(&board, 0x020, 0x100);                                                   /* BG2PA */
    agb_ppu_write(&board, 0x026, 0x100);                                                   /* BG2PD */
    agb_ppu_write(&board, 0x000, 0x0401);
    agb_ppu_write(&board, 0x00C, 0x1E40); /* BG2: screen base 30, mosaic */
    agb_ppu_write(&board, 0x04C, 0x0003);
    run_frame();
    CHECK(pixel(1, 0) == PIX(31, 0, 0));
    CHECK(pixel(4, 0) == PIX(0, 0, 31));
    agb_ppu_write(&board, 0x04C, 0x0010);
    run_frame();
    CHECK(pixel(1, 1) == PIX(0, 31, 0)); /* line 1 walks from line 0's reference point */
    agb_ppu_write(&board, 0x00C, 0x1E00);
    run_frame();
    CHECK(pixel(1, 1) == PIX(0, 0, 31)); /* row 1 without it */

    /* a bitmap: mode 3's BG2 */
    memset(board.memory.vram, 0, AGB_VRAM_SIZE);
    agb_write16(&board, 0x06000000, 0x001F);
    agb_write16(&board, 0x06000002, 0x03E0);
    agb_write16(&board, 0x06000000 + 240 * 2, 0x7C00);
    agb_write16(&board, 0x06000002 + 240 * 2, 0x7C00);
    agb_ppu_write(&board, 0x000, 0x0403);
    agb_ppu_write(&board, 0x00C, 0x0040);
    agb_ppu_write(&board, 0x04C, 0x0001); /* 2 pixels wide */
    run_frame();
    CHECK(pixel(1, 0) == PIX(31, 0, 0));
    agb_ppu_write(&board, 0x04C, 0x0010);
    run_frame();
    CHECK(pixel(1, 1) == PIX(0, 31, 0));
    agb_ppu_write(&board, 0x00C, 0);

    /* a sprite at x 10: its pixel 0 red, 2 green and 6 blue. OBJ blocks 4 wide sit on the screen's grid (8, 12, 16),
       so x 10 and 11 show its pixel 0 (held to the sprite's first), 12 to 15 pixel 2, and 16 to 19 pixel 6, two
       columns past its right edge */
    memset(board.memory.vram, 0, AGB_VRAM_SIZE);
    agb_write16(&board, 0x05000202, 0x001F);
    agb_write16(&board, 0x05000204, 0x03E0);
    agb_write16(&board, 0x05000206, 0x7C00);
    agb_write16(&board, 0x06010000, 0x0201); /* pixels 0 to 3: 1 0 2 0 */
    agb_write16(&board, 0x06010002, 0x0300); /* pixels 4 to 7: 0 0 3 0 */
    agb_write16(&board, 0x07000000, 0x1000); /* y 0, mosaic */
    agb_write16(&board, 0x07000002, 10);
    agb_ppu_write(&board, 0x000, 0x1000); /* OBJ on */
    agb_ppu_write(&board, 0x04C, 0x0300);
    run_frame();
    CHECK(pixel(11, 0) == PIX(31, 0, 0));
    CHECK(pixel(13, 0) == PIX(0, 31, 0));
    CHECK(pixel(17, 0) == PIX(0, 0, 31));
    CHECK(pixel(19, 0) == PIX(0, 0, 31));
    CHECK(pixel(20, 0) == PIX(0, 0, 0));
    agb_write16(&board, 0x07000000, 0x0000);
    run_frame();
    CHECK(pixel(11, 0) == PIX(0, 0, 0)); /* the sprite's own pixel 1 without it */
    CHECK(pixel(19, 0) == PIX(0, 0, 0));
    agb_write16(&board, 0x07000000, 0x1000);
    agb_ppu_write(&board, 0x04C, 0x1000); /* OBJ blocks 2 lines high: line 1 shows row 0 */
    run_frame();
    CHECK(pixel(10, 1) == PIX(31, 0, 0));
    /* the same sprite affine, no rotation: a block that starts left of it samples the pixel before it, which is off
       the sprite, so x 10 and 11 stay open */
    agb_write16(&board, 0x07000000, 0x1100);
    agb_write16(&board, 0x07000006, 0x0100); /* group 0: pa */
    agb_write16(&board, 0x0700001E, 0x0100); /* pd */
    agb_ppu_write(&board, 0x04C, 0x0300);
    run_frame();
    CHECK(pixel(10, 0) == PIX(0, 0, 0));
    CHECK(pixel(13, 0) == PIX(0, 31, 0));
    CHECK(pixel(19, 0) == PIX(0, 0, 31));
    agb_write16(&board, 0x07000000, 0x0100);
    run_frame();
    CHECK(pixel(10, 0) == PIX(31, 0, 0));
    /* stretched by 4/3 (pa 0xC0), the pixel before the sprite lands on its pixel 0, red, which x 10 shows; without
       mosaic x 10 is its pixel 1 */
    agb_write16(&board, 0x07000006, 0x00C0);
    run_frame();
    CHECK(pixel(10, 0) == PIX(0, 0, 0));
    agb_write16(&board, 0x07000000, 0x1100);
    run_frame();
    CHECK(pixel(10, 0) == PIX(31, 0, 0));
    CHECK(pixel(13, 0) == PIX(0, 31, 0));
    memset(board.memory.oam, 0, AGB_OAM_SIZE);
    agb_ppu_write(&board, 0x04C, 0);
    agb_ppu_write(&board, 0x000, 0x0100);

    /* two backgrounds of one priority: the lower number goes over the higher, BG0's red over BG1's green */
    memset(board.memory.vram, 0, AGB_VRAM_SIZE);
    memset(board.memory.palette, 0, AGB_PALETTE_SIZE);
    agb_write16(&board, 0x05000002, 0x001F);
    agb_write16(&board, 0x05000004, 0x03E0);
    agb_write16(&board, 0x06000000, 0x1111); /* char base 0, tile 0's row 0: entry 1 */
    agb_write16(&board, 0x06000002, 0x1111);
    agb_write16(&board, 0x06004000, 0x2222); /* char base 1: entry 2 */
    agb_write16(&board, 0x06004002, 0x2222);
    agb_ppu_write(&board, 0x008, 0x1F01); /* BG0: priority 1, screen base 31 */
    agb_ppu_write(&board, 0x00A, 0x1E05); /* BG1: priority 1, char base 1, screen base 30 */
    agb_ppu_write(&board, 0x000, 0x0300);
    run_frame();
    CHECK(pixel(0, 0) == PIX(31, 0, 0));
    agb_ppu_write(&board, 0x00A, 0x1E04); /* BG1 at priority 0 goes over it */
    run_frame();
    CHECK(pixel(0, 0) == PIX(0, 31, 0));
    agb_ppu_write(&board, 0x00A, 0);

    /* a sprite's open pixel lends its priority to the one an earlier sprite drew there: sprite 0, priority 3, under
       BG0 (priority 1), comes over it where sprite 1, priority 0, is open */
    agb_write16(&board, 0x05000202, 0x7C00);  /* OBJ entry 1: blue */
    agb_write16(&board, 0x06010000, 0x0001);  /* tile 0's pixel 0; tile 1 open */
    agb_write16(&board, 0x07000000, 0x0000);  /* sprite 0: y 0 */
    agb_write16(&board, 0x07000002, 0);       /* x 0 */
    agb_write16(&board, 0x07000004, 3 << 10); /* tile 0, priority 3 */
    for (int i = 1; i < 128; ++i) agb_write16(&board, 0x07000000 + (uint32_t)i * 8, 0x0200); /* the rest hidden */
    agb_ppu_write(&board, 0x000, 0x1100);
    run_frame();
    CHECK(pixel(0, 0) == PIX(31, 0, 0)); /* BG0 over sprite 0 */
    agb_write16(&board, 0x07000008, 0x0000);
    agb_write16(&board, 0x0700000A, 0);
    agb_write16(&board, 0x0700000C, 1); /* tile 1, priority 0 */
    run_frame();
    CHECK(pixel(0, 0) == PIX(0, 0, 31));
    memset(board.memory.oam, 0, AGB_OAM_SIZE);
    agb_ppu_write(&board, 0x008, 0);
    agb_ppu_write(&board, 0x000, 0x0100);

    /* a window's lines latch as the line counter meets its top and bottom, as mGBA's do: a WIN0V top of 231 is never
       met, so with its bottom at 195 the window stays off, where a range would wrap over every line; a top of 100 and
       a bottom of 50 hold it on from line 100 through the next frame's line 49 */
    memset(board.memory.vram, 0, AGB_VRAM_SIZE);
    memset(board.memory.palette, 0, AGB_PALETTE_SIZE);
    for (int i = 0; i < 128; ++i) agb_write16(&board, 0x07000000 + (uint32_t)i * 8, 0x0200);
    agb_write16(&board, 0x05000002, 0x001F);
    for (uint32_t i = 0; i < 32; i += 2) agb_write16(&board, 0x06000000 + i, 0x1111); /* tile 0: entry 1, red */
    agb_ppu_write(&board, 0x008, 0x1F00);
    agb_ppu_write(&board, 0x040, 0x00F0); /* WIN0: x 0 to 240 */
    agb_ppu_write(&board, 0x044, 0xE7C3);
    agb_ppu_write(&board, 0x048, 0x0000); /* inside: nothing */
    agb_ppu_write(&board, 0x04A, 0x0001); /* outside: BG0 */
    agb_ppu_write(&board, 0x000, 0x2100);
    run_frame();
    run_frame();
    CHECK(pixel(10, 10) == PIX(31, 0, 0) && pixel(10, 150) == PIX(31, 0, 0));
    agb_ppu_write(&board, 0x044, 100 << 8 | 50);
    run_frame();
    run_frame();
    CHECK(pixel(10, 10) == PIX(0, 0, 0) && pixel(10, 70) == PIX(31, 0, 0) && pixel(10, 120) == PIX(0, 0, 0));
    for (uint32_t r = 0x040; r <= 0x04A; r += 2) agb_ppu_write(&board, r, 0);
    agb_ppu_write(&board, 0x008, 0);
    agb_ppu_write(&board, 0x000, 0x0100);

    /* the backdrop alone has nothing under it to blend with, though BLDCNT names it both targets */
    agb_write16(&board, 0x05000000, 0x7FFF);
    agb_ppu_write(&board, 0x000, 0x0000); /* no layer: the backdrop everywhere */
    agb_ppu_write(&board, 0x050, 0x2020 | 1 << 6);
    agb_ppu_write(&board, 0x052, 0x0404);
    run_frame();
    CHECK(pixel(10, 10) == PIX(31, 31, 31));
    agb_ppu_write(&board, 0x050, 0);
    agb_ppu_write(&board, 0x052, 0);
    agb_write16(&board, 0x05000000, 0);
    agb_ppu_write(&board, 0x000, 0x0100);

    /* the blend weights hold to 16 sixteenths past it: BG0's red 8 at 31/16 over the backdrop stays 8, and darkened by
       31/16 goes black */
    agb_write16(&board, 0x05000002, 0x0008);
    agb_ppu_write(&board, 0x008, 0x1F00); /* BG0: tile 0, entry 1, everywhere */
    agb_ppu_write(&board, 0x000, 0x0100);
    agb_ppu_write(&board, 0x050, 0x2001 | 1 << 6);
    agb_ppu_write(&board, 0x052, 31);
    run_frame();
    CHECK(pixel(10, 10) == PIX(8, 0, 0));
    agb_ppu_write(&board, 0x050, 0x0001 | 3 << 6);
    agb_ppu_write(&board, 0x054, 31);
    run_frame();
    CHECK(pixel(10, 10) == PIX(0, 0, 0));
    agb_ppu_write(&board, 0x050, 0);
    agb_ppu_write(&board, 0x052, 0);
    agb_ppu_write(&board, 0x054, 0);
    agb_ppu_write(&board, 0x008, 0);

    /* a semi transparent sprite with nothing to blend with below takes the brightness effect when OBJ is a first target,
       or when another layer shown is a second one (BG0 here, empty): green 31 over the backdrop lights to white, or
       darkens to 15 */
    memset(board.memory.vram, 0, AGB_VRAM_SIZE);
    agb_write16(&board, 0x05000202, 0x03E0);                                          /* OBJ entry 1: green */
    for (uint32_t i = 0; i < 32; i += 2) agb_write16(&board, 0x06010020 + i, 0x1111); /* OBJ tile 1 */
    agb_write16(&board, 0x07000000, 0x0400); /* sprite 0: semi transparent, y 0 */
    agb_write16(&board, 0x07000002, 100);
    agb_write16(&board, 0x07000004, 1);
    agb_ppu_write(&board, 0x008, 0x1F00);
    agb_ppu_write(&board, 0x000, 0x1140);          /* BG0 and OBJ, 1D */
    agb_ppu_write(&board, 0x050, 0x0120 | 2 << 6); /* the backdrop first, BG0 second, brighten */
    agb_ppu_write(&board, 0x054, 16);
    run_frame();
    CHECK(pixel(100, 0) == PIX(31, 31, 31));
    agb_ppu_write(&board, 0x050, 0x0010 | 3 << 6); /* OBJ first, no second, darken */
    agb_ppu_write(&board, 0x054, 8);
    run_frame();
    CHECK(pixel(100, 0) == PIX(0, 16, 0));
    agb_ppu_write(&board, 0x050, 0x0020 | 2 << 6); /* neither: as it is */
    run_frame();
    CHECK(pixel(100, 0) == PIX(0, 31, 0));
    agb_ppu_write(&board, 0x050, 0);
    agb_ppu_write(&board, 0x054, 0);
    agb_write16(&board, 0x07000000, 0x0200);
    agb_ppu_write(&board, 0x008, 0);
    agb_ppu_write(&board, 0x000, 0x0100);

    /* a semi transparent sprite blends with a second target below inside a window that lets no effect through: green
       over BG0's red at 8 and 8 sixteenths, in WIN0 */
    agb_write16(&board, 0x05000002, 0x001F);
    for (uint32_t i = 0; i < 32; i += 2) agb_write16(&board, 0x06000000 + i, 0x1111); /* BG0 tile 0: red */
    agb_write16(&board, 0x07000000, 0x0400);
    agb_ppu_write(&board, 0x008, 0x1F00);
    agb_ppu_write(&board, 0x040, 90 << 8 | 110);
    agb_ppu_write(&board, 0x044, 0 << 8 | 8);
    agb_ppu_write(&board, 0x048, 0x0011); /* inside: BG0 and OBJ, no effect */
    agb_ppu_write(&board, 0x04A, 0x0031);
    agb_ppu_write(&board, 0x000, 0x3140);
    agb_ppu_write(&board, 0x050, 0x0100);
    agb_ppu_write(&board, 0x052, 0x0808);
    run_frame();
    run_frame();
    CHECK(pixel(100, 0) == PIX(15, 15, 0));
    for (uint32_t r = 0x040; r <= 0x04A; r += 2) agb_ppu_write(&board, r, 0);
    agb_ppu_write(&board, 0x050, 0);
    agb_ppu_write(&board, 0x052, 0);
    agb_write16(&board, 0x07000000, 0x0200);
    agb_ppu_write(&board, 0x008, 0);
    agb_ppu_write(&board, 0x000, 0x0100);

    /* a bitmap goes through BG2's rotation and scaling: halved across (PA 2.0) its pixel 2 shows at x 1, moved 8 down
       (BG2Y 8.0) its line 8 shows at line 0, and past its edge the backdrop shows */
    memset(board.memory.vram, 0, AGB_VRAM_SIZE);
    agb_write16(&board, 0x05000000, 0x7C00);               /* the backdrop: blue */
    agb_write16(&board, 0x06000000 + 2 * 2, 0x001F);       /* (2, 0): red */
    agb_write16(&board, 0x06000000 + 8 * 480 + 4, 0x03E0); /* (2, 8): green */
    agb_ppu_write(&board, 0x000, 0x0403);
    agb_ppu_write(&board, 0x020, 0x200);
    run_frame();
    CHECK(pixel(1, 0) == PIX(31, 0, 0) && pixel(120, 0) == PIX(0, 0, 31)); /* x 120 reads past 239 */
    agb_write16(&board, 0x0400002C, 8 << 8);                               /* BG2Y */
    agb_write16(&board, 0x0400002E, 0);
    run_frame();
    CHECK(pixel(1, 0) == PIX(0, 31, 0));
    agb_write16(&board, 0x0400002C, 0);
    agb_ppu_write(&board, 0x020, 0x100);
    agb_write16(&board, 0x05000000, 0);
    agb_ppu_write(&board, 0x000, 0x0100);
    run_frame();

    /* an HBlank DMA runs on the 160 visible lines alone, a game's table having one entry a line: a repeating 16 bit one
       moves its source 160 halfwords a frame, not 228 */
    agb_write16(&board, 0x040000B0, 0x0000);
    agb_write16(&board, 0x040000B2, 0x0200); /* DMA0SAD: 0x02000000 */
    agb_write16(&board, 0x040000B4, 0x0010);
    agb_write16(&board, 0x040000B6, 0x0400); /* DMA0DAD: BG0HOFS */
    agb_write16(&board, 0x040000B8, 1);
    agb_write16(&board, 0x040000BA, 0xA240); /* on, at HBlank, repeating, the destination fixed */
    run_frame();
    CHECK(board.dma[0].source_now == 0x02000000 + 160 * 2);
    agb_write16(&board, 0x040000BA, 0);
    agb_board_destroy(&board);
    puts("agb_ppu: ok");
    return 0;
}
