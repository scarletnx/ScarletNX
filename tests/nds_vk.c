/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The NDS's picture on the Vulkan device, with no window: a background of one tile composed by the 2D engines' pass, a
   triangle drawn by the 3D engine's and shown as BG0 (filled, as a wireframe, as a line, anti-aliased, and over a
   clear bitmap), a display capture read back into its VRAM bank, an object under a mosaic and the main memory
   display. The frames
   are read back and their pixels compared. No game's data. Exits 77 where the host has no Vulkan 1.3 device. */
#include "nds/board/nds.h"
#include "nds/gpu/gpu3d.h"
#include "nds/gpu/vk/vk.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "support/nds_image.h"
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
static unsigned char g_frame[NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT * 2 * 4];

/* a frame of the board's time, with no CPU: the lines' registers kept, the frame composed, the 3D picture drawn */
static void frame(NdsBoard* board) {
    board->frame_done = 0;
    while (!board->frame_done) {
        board->cycles = board->next_event;
        nds_run_events(board);
    }
    /* on to the line the 3D engine draws at */
    while (board->vcount != 216) {
        board->cycles = board->next_event;
        nds_run_events(board);
    }
}
static const unsigned char* pixel(unsigned screen, unsigned x, unsigned y) {
    return g_frame + ((screen * NDS_SCREEN_HEIGHT + y) * NDS_SCREEN_WIDTH + x) * 4;
}
static bool is_color(const unsigned char* p, unsigned r, unsigned g, unsigned b) {
    return p[0] == r && p[1] == g && p[2] == b;
}
static void gx(NdsBoard* board, uint32_t command, uint32_t parameter) {
    nds_io_write(board, IO + 0x400 + (command << 2 & 0x1FC), 4, parameter);
}

/* the test's triangle, blue, drawn and shown: its last vertex at (x, y), where the whole triangle has (0, 0.5) */
static void triangle(NdsBoard* board, uint32_t attributes, uint16_t x, uint16_t y) {
    gx(board, 0x29, attributes);
    gx(board, 0x40, 0);
    gx(board, 0x20, 0x7C00);
    gx(board, 0x25, (uint16_t)-0x800 | (uint32_t)(uint16_t)-0x800 << 16);
    gx(board, 0x25, 0x800 | (uint32_t)(x == 0x800 && y == 0x800 ? 0x800 : (uint16_t)-0x800) << 16);
    gx(board, 0x25, x | (uint32_t)y << 16);
    gx(board, 0x41, 0);
    gx(board, 0x50, 0);
    frame(board);
    frame(board);
    CHECK(nds_vk_frame_read(g_frame));
}
/* a line's pixel is at (x, y) or beside it */
static bool blue_near(unsigned x, unsigned y) {
    for (unsigned k = x - 1; k <= x + 1; ++k)
        if (is_color(pixel(0, k, y), 0, 0, 255) || is_color(pixel(0, k, y - 1), 0, 0, 255)) return true;
    return false;
}

int main(void) {
    char error[512] = "";
    if (!nds_vk_init("nds test", true, error, sizeof error)) {
        printf("nds_vk: skipped, %s\n", error);
        return 77;
    }
    CHECK(nds_vk_render3d_init(error, sizeof error) && nds_vk_frame_init(error, sizeof error) &&
          nds_vk_present_init(error, sizeof error));
    char* dir = snx_temp_dir(NULL);
    char* image = dir ? snx_path_join(dir, "test.nds") : NULL;
    CHECK(image && nds_test_image(image, nds_test_program, nds_test_program_words));
    NdsRom rom;
    CHECK(nds_rom_open(&rom, image, NULL, error, sizeof error));
    NdsBoard* board = calloc(1, sizeof *board);
    CHECK(board && nds_board_init(board, &rom, error, sizeof error));
    unsigned char* w = board->mem.base;

    /* engine A on the upper screen: BG1 a text background of 16 colors, whose tile 0 is all color 1, red; bank A is
       its memory. Engine B shows nothing: white */
    nds_io_write(board, IO + 0x304, 2, 0x820F);
    nds_io_write(board, IO + 0x240, 1, 0x81);
    snx_st_le16(w + 0x05000002, 0x001F);            /* color 1: red */
    snx_st_le16(w + 0x05000004, 0x03E0);            /* color 2: green */
    memset(w + 0x06004000, 0x11, 32);               /* tile 0 of the tile set at 0x4000 */
    memset(w + 0x06004020, 0x22, 32);               /* tile 1: green */
    snx_st_le16(w + 0x06000000 + (4 * 32 + 5) * 2, 1); /* the map's cell (5, 4) is tile 1 */
    nds_io_write(board, IO + 0x00A, 2, 1 << 2 | 1); /* BG1: the tiles at 0x4000, the map at 0, priority 1 */
    nds_io_write(board, IO + 0x000, 4, 0x00010200); /* mode 0, BG1, the graphics display */
    frame(board);
    CHECK(nds_vk_frame_read(g_frame));
    CHECK(is_color(pixel(0, 0, 0), 251, 0, 0) && is_color(pixel(0, 255, 191), 251, 0, 0));
    CHECK(is_color(pixel(0, 44, 36), 0, 251, 0) && is_color(pixel(0, 39, 36), 251, 0, 0)); /* the cell at (40, 32) */
    CHECK(is_color(pixel(1, 128, 96), 255, 255, 255));
    /* the 3D picture as BG0, in front: a blue triangle over the middle, nothing elsewhere */
    nds_io_write(board, IO + 0x060, 2, 0);
    nds_io_write(board, IO + 0x350, 4, 0); /* the clear plane: no alpha */
    gx(board, 0x10, 0), gx(board, 0x15, 0);
    gx(board, 0x10, 2), gx(board, 0x15, 0);
    gx(board, 0x60, 0 | 0 << 8 | 255u << 16 | 191u << 24);
    gx(board, 0x29, 0x001F00C0);
    gx(board, 0x40, 0);
    gx(board, 0x20, 0x7C00);
    gx(board, 0x25, (uint16_t)-0x800 | (uint32_t)(uint16_t)-0x800 << 16);
    gx(board, 0x25, 0x800 | (uint32_t)(uint16_t)-0x800 << 16);
    gx(board, 0x25, 0 | 0x800u << 16);
    gx(board, 0x41, 0);
    gx(board, 0x50, 0);
    nds_io_write(board, IO + 0x008, 2, 0);          /* BG0: priority 0 */
    nds_io_write(board, IO + 0x000, 4, 0x00010308); /* BG0 the 3D picture, and BG1 */
    frame(board); /* the buffers swap and the triangle is drawn */
    frame(board); /* and shown */
    CHECK(nds_vk_frame_read(g_frame));
    /* inside the triangle: a vertex color's 31 is the 3D engine's full 63, where a palette's is 62 */
    CHECK(is_color(pixel(0, 128, 110), 0, 0, 255));
    CHECK(is_color(pixel(0, 10, 10), 251, 0, 0));   /* outside: the background */
    CHECK(is_color(pixel(0, 128, 30), 251, 0, 0));  /* above its top */

    /* the display capture: the frame's 256 by 192 into bank B, as 15-bit pixels with the top bit set */
    nds_io_write(board, IO + 0x241, 1, 0x80);
    nds_io_write(board, IO + 0x064, 4, 0x80000000u | 3u << 20 | 1u << 16);
    frame(board);
    CHECK(!(nds_io_read(board, IO + 0x064, 4) >> 31));
    nds_vk_capture_land(board);
    const unsigned char* bank = board->mem.vram[1].view;
    CHECK(snx_ld_le16(bank + (10 * 256 + 10) * 2) == 0x801F);   /* red */
    CHECK(snx_ld_le16(bank + (110 * 256 + 128) * 2) == 0xFC00); /* blue */

    /* the triangle again, as the 3D engine's other ways of drawing have it. Its corners are (64, 144), (192, 144) and
       (128, 48) */
    /* a wireframe (the polygon's alpha 0): its outline alone, one pixel wide */
    triangle(board, 0x000000C0, 0x800, (uint16_t)-0x800);
    CHECK(is_color(pixel(0, 128, 143), 0, 0, 255));  /* its lowest row */
    CHECK(is_color(pixel(0, 128, 141), 251, 0, 0));  /* two rows up: inside, where nothing is drawn */
    CHECK(is_color(pixel(0, 128, 110), 251, 0, 0));
    /* a line (two of the three vertices at one place), from (64, 144) to (192, 48) */
    triangle(board, 0x001F00C0, 0x800, 0x800);
    CHECK(blue_near(128, 96) && blue_near(96, 120) && blue_near(160, 72));
    CHECK(is_color(pixel(0, 128, 130), 251, 0, 0) && is_color(pixel(0, 128, 60), 251, 0, 0));
    /* anti-aliasing: the first pixel of the triangle in a row that crosses its slanted side is part triangle and part
       what is under it. The picture's alpha says how much, so BG1 is BLDCNT's second target */
    nds_io_write(board, IO + 0x050, 2, 0x0200);
    for (unsigned smooth = 0; smooth < 2; ++smooth) {
        nds_io_write(board, IO + 0x060, 2, smooth ? 0x0010 : 0);
        triangle(board, 0x001F00C0, 0, 0x800);
        unsigned x = 0, mixed = 0;
        while (x < 128 && !pixel(0, x, 97)[2]) ++x;
        CHECK(x > 80 && x < 112);
        for (unsigned y = 60; y < 140; ++y)
            for (unsigned k = 64; k < 128; ++k) mixed += pixel(0, k, y)[0] && pixel(0, k, y)[2];
        CHECK(smooth ? mixed >= 16 : !mixed);
        CHECK(is_color(pixel(0, 128, 110), 0, 0, 255) && is_color(pixel(0, 10, 10), 251, 0, 0));
    }
    nds_io_write(board, IO + 0x050, 2, 0);
    /* the clear bitmap: texture slots 2 and 3 (banks C and D) hold the color and the depth of a 256 by 256 picture the
       frame starts from. All of it is green and far, but for one pixel that is not there and one that is near */
    for (unsigned i = 0; i < 0x10000; ++i) {
        snx_st_le16(board->mem.vram[2].view + i * 2, 0x83E0);
        snx_st_le16(board->mem.vram[3].view + i * 2, 0x7FFF);
    }
    snx_st_le16(board->mem.vram[2].view + (20 * 256 + 20) * 2, 0x03E0);
    snx_st_le16(board->mem.vram[3].view + (110 * 256 + 128) * 2, 0x0000);
    nds_io_write(board, IO + 0x242, 1, 0x93);
    nds_io_write(board, IO + 0x243, 1, 0x9B);
    nds_io_write(board, IO + 0x060, 2, 0x4000);
    triangle(board, 0x001F00C0, 0, 0x800);
    CHECK(is_color(pixel(0, 10, 10), 0, 255, 0));    /* the bitmap */
    CHECK(is_color(pixel(0, 20, 20), 251, 0, 0));    /* its pixel that is not there: the background */
    CHECK(is_color(pixel(0, 128, 100), 0, 0, 255));  /* the triangle before it */
    CHECK(is_color(pixel(0, 128, 110), 0, 255, 0));  /* and behind its near pixel */
    nds_io_write(board, IO + 0x356, 2, 8 | 2 << 8);  /* the bitmap moved 8 left and 2 up */
    triangle(board, 0x001F00C0, 0, 0x800);
    CHECK(is_color(pixel(0, 12, 18), 251, 0, 0) && is_color(pixel(0, 20, 20), 0, 255, 0));
    CHECK(is_color(pixel(0, 120, 108), 0, 255, 0) && is_color(pixel(0, 128, 110), 0, 0, 255));
    nds_io_write(board, IO + 0x060, 2, 0);

    /* an object of engine A, 8 by 8 at (100, 50): its first column red, the rest green. With its mosaic bit and a
       mosaic 4 wide, each block of 4 shows the object's pixel at the block's start */
    nds_io_write(board, IO + 0x241, 1, 0x82); /* bank B: engine A's objects */
    snx_st_le16(w + 0x05000202, 0x001F);
    snx_st_le16(w + 0x05000204, 0x03E0);
    for (unsigned row = 0; row < 8; ++row) snx_st_le32(w + 0x06400000 + row * 4, 0x22222221);
    for (unsigned n = 0; n < 128; ++n) snx_st_le16(w + 0x07000000 + n * 8, 0x0200); /* the others not shown */
    snx_st_le16(w + 0x07000000, 50);
    snx_st_le16(w + 0x07000002, 100);
    snx_st_le16(w + 0x07000004, 0);
    nds_io_write(board, IO + 0x000, 4, 0x00011200); /* BG1 and the objects */
    nds_io_write(board, IO + 0x04C, 2, 3 << 8);
    frame(board);
    CHECK(nds_vk_frame_read(g_frame));
    CHECK(is_color(pixel(0, 100, 50), 251, 0, 0) && is_color(pixel(0, 101, 50), 0, 251, 0)); /* no mosaic bit */
    snx_st_le16(w + 0x07000000, 50 | 0x1000);
    frame(board);
    CHECK(nds_vk_frame_read(g_frame));
    CHECK(is_color(pixel(0, 101, 50), 251, 0, 0) && is_color(pixel(0, 103, 50), 251, 0, 0));
    CHECK(is_color(pixel(0, 104, 50), 0, 251, 0) && is_color(pixel(0, 107, 57), 0, 251, 0));
    nds_io_write(board, IO + 0x04C, 2, 0);
    /* the main memory display: a DMA channel sends a frame from main memory to the display's FIFO, a line at each
       visible line. The frame is blue with a green pixel at (7, 3) */
    for (unsigned i = 0; i < 256 * 192; ++i) snx_st_le16(w + 0x02100000 + i * 2, 0x7C00);
    snx_st_le16(w + 0x02100000 + (3 * 256 + 7) * 2, 0x03E0);
    nds_io_write(board, IO + 0x0B0, 4, 0x02100000);
    nds_io_write(board, IO + 0x0B4, 4, IO + 0x068);
    nds_io_write(board, IO + 0x0B8, 4, 0x80000000u | 4u << 27 | 1u << 26 | 2u << 21 | 0x6000);
    nds_io_write(board, IO + 0x000, 4, 0x00030000);
    frame(board);
    frame(board);
    CHECK(nds_vk_frame_read(g_frame));
    CHECK(is_color(pixel(0, 0, 0), 0, 0, 251) && is_color(pixel(0, 255, 191), 0, 0, 251));
    CHECK(is_color(pixel(0, 7, 3), 0, 251, 0) && is_color(pixel(0, 8, 3), 0, 0, 251));

    nds_board_free(board);
    free(board);
    nds_rom_close(&rom);
    nds_vk_present_free();
    nds_vk_frame_free();
    nds_vk_render3d_free();
    nds_vk_free();
    (void)snx_remove_tree(dir);
    free(image);
    free(dir);
    puts("nds_vk: ok");
    return 0;
}
