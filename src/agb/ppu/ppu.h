/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
/* the AGB display device: the LCD controller's registers and timing, and the PPU that draws the
   backgrounds and sprites of every scanline into the frame the host presents. */
#ifndef SNX_AGB_PPU_H
#define SNX_AGB_PPU_H
#include <stdint.h>

#define AGB_SCREEN_WIDTH    240
#define AGB_SCREEN_HEIGHT   160
#define AGB_SCANLINE_CYCLES 1232 /* 308 dots of 4 cycles: 240 visible, then the horizontal blank */
#define AGB_SCANLINE_DOTS   308
#define AGB_FRAME_LINES     228 /* 160 visible lines, then the vertical blank */

typedef struct AgbBoard AgbBoard;
/* The scanline buffers the composite pass reads, which the PPU draws one line at a time */
#define AGB_LAYERS 6 /* BG0 to BG3, OBJ and the backdrop */
typedef struct {
    uint16_t color[AGB_LAYERS][AGB_SCREEN_WIDTH]; /* an unused colour bit marks a layer's open pixels */
    uint8_t obj_pri[AGB_SCREEN_WIDTH];
    uint8_t obj_mode[AGB_SCREEN_WIDTH]; /* the mode of the sprite that drew the pixel */
    uint8_t objwin[AGB_SCREEN_WIDTH];   /* the OBJ window layer */
    uint8_t drawn[AGB_SCREEN_WIDTH];    /* a sprite already owns the pixel */
} AgbScanline;
typedef struct {
    uint16_t dispcnt, greenswap;
    uint16_t bgcnt[4], bghofs[4], bgvofs[4];
    uint16_t bgpa[4], bgpb[4], bgpc[4], bgpd[4]; /* the affine parameters of BG2 and BG3 */
    int32_t bgx[4], bgy[4];                      /* the reference points the CPU writes */
    int32_t bgx_line[4], bgy_line[4];            /* the reference points the line renderer walks */
    uint16_t win0h, win1h, win0v, win1v, winin, winout, mosaic;
    uint8_t window_on[2]; /* WIN0's and WIN1's lines: on from the line the counter meets its top to its bottom's */
    uint16_t bldcnt, bldalpha, bldy;
    uint16_t dispstat;
    uint16_t vcount;
    uint32_t dot;                                               /* cycles into the current scanline */
    uint32_t framebuffer[AGB_SCREEN_WIDTH * AGB_SCREEN_HEIGHT]; /* 0xAABBGGRR words, one scanline at a time */
    uint8_t lcd;      /* gba_color: 1 as the original GBA's screen shows the colours (gba), 0 as written (raw) */
    AgbScanline scan; /* the line being drawn */
} AgbPpu;

/* The register window at 0x04000000 to 0x0400005F, halfword addressed. */
uint16_t agb_ppu_read(AgbBoard* board, uint32_t offset);
void agb_ppu_write(AgbBoard* board, uint32_t offset, uint16_t value);
/* The frame's colours: lcd 0 widens each channel's five bits to eight (raw), 1 shows them as the original GBA's unlit
   screen does (gba, gba_color's) */
void agb_ppu_lcd(AgbBoard* board, int lcd);
/* Runs the display for the given CPU cycles: scanlines, the blanking flags and the interrupts. */
void agb_ppu_step(AgbBoard* board, uint32_t cycles);
#endif
