/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
#include "agb/ppu/ppu.h"
#include "agb/board/board.h"
#include <string.h>

/* The scanline buffers the composite pass reads. The board renders one line at a time, on one thread. */
#define LAYER_TRANSPARENT 0x8000 /* an unused bit of the 15 bit palette colour */
#define LAYERS            6      /* BG0 to BG3, OBJ and the backdrop */
typedef struct {
    uint16_t color[LAYERS][AGB_SCREEN_WIDTH]; /* LAYER_TRANSPARENT marks the pixels a layer leaves open */
    uint8_t obj_pri[AGB_SCREEN_WIDTH];
    uint8_t obj_mode[AGB_SCREEN_WIDTH]; /* the mode of the sprite that drew the pixel */
    uint8_t objwin[AGB_SCREEN_WIDTH];   /* the OBJ window layer */
    uint8_t drawn[AGB_SCREEN_WIDTH];    /* a sprite already owns the pixel */
} AgbScanline;
static AgbScanline scan;

static uint16_t vram16(const AgbBoard* board, uint32_t offset) {
    const uint8_t* p = board->memory.vram + (offset & 0x1FFFE);
    return (uint16_t)(p[0] | p[1] << 8);
}
static uint16_t palette(const AgbBoard* board, uint32_t index) {
    const uint8_t* p = board->memory.palette + (index & 0x1FF) * 2;
    return (uint16_t)((p[0] | p[1] << 8) & 0x7FFF);
}
static uint16_t oam16(const AgbBoard* board, uint32_t offset) {
    const uint8_t* p = board->memory.oam + (offset & 0x3FE);
    return (uint16_t)(p[0] | p[1] << 8);
}
static uint32_t rgb888(uint16_t color) {
    uint32_t r = (uint32_t)(color & 31), g = (uint32_t)((color >> 5) & 31), b = (uint32_t)((color >> 10) & 31);
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return r | (g << 8) | (b << 16) | (0xFFu << 24);
}

static void render_line(AgbBoard* board, int y);

/* ---- registers ---- */

/* BG2 and BG3 store their affine parameters as four halfwords each. */
static uint16_t* affine_param(AgbPpu* ppu, uint32_t offset) {
    uint32_t index = offset >= 0x030 ? 3 : 2;
    switch (offset & 6) {
        case 0: return &ppu->bgpa[index];
        case 2: return &ppu->bgpb[index];
        case 4: return &ppu->bgpc[index];
        default: return &ppu->bgpd[index];
    }
}

/* The reference points are signed 28 bit values written through two halfwords. */
static void reference_write(AgbPpu* ppu, uint32_t offset, uint16_t value) {
    uint32_t index = offset >= 0x038 ? 3 : 2, y_axis = (offset & 0x04) ? 1u : 0u, shift = (offset & 2) * 8;
    int32_t* written = y_axis ? &ppu->bgy[index] : &ppu->bgx[index];
    int32_t* line = y_axis ? &ppu->bgy_line[index] : &ppu->bgx_line[index];
    uint32_t combined = ((uint32_t)*written & ~(0xFFFFu << shift)) | ((uint32_t)value << shift);
    *written = (int32_t)(combined << 4) >> 4;
    *line = *written;
}

static uint16_t reference_read(const AgbPpu* ppu, uint32_t offset) {
    uint32_t index = offset >= 0x038 ? 3 : 2, y_axis = (offset & 0x04) ? 1u : 0u;
    int32_t value = y_axis ? ppu->bgy[index] : ppu->bgx[index];
    return (uint16_t)((uint32_t)value >> ((offset & 2) * 8));
}

uint16_t agb_ppu_read(AgbBoard* board, uint32_t offset) {
    AgbPpu* ppu = &board->ppu;
    switch (offset) {
        case 0x000: return ppu->dispcnt;
        case 0x002: return ppu->greenswap;
        case 0x004: return ppu->dispstat;
        case 0x006: return ppu->vcount;
        case 0x040: return ppu->win0h;
        case 0x042: return ppu->win1h;
        case 0x044: return ppu->win0v;
        case 0x046: return ppu->win1v;
        case 0x048: return ppu->winin;
        case 0x04A: return ppu->winout;
        case 0x04C: return ppu->mosaic;
        case 0x050: return ppu->bldcnt;
        case 0x052: return ppu->bldalpha;
        case 0x054: return ppu->bldy;
        default: break;
    }
    if (offset >= 0x008 && offset <= 0x00E) return ppu->bgcnt[(offset - 0x008) >> 1];
    if (offset >= 0x010 && offset <= 0x01E) {
        uint32_t n = (offset - 0x010) >> 2;
        return (offset & 2) ? ppu->bgvofs[n] : ppu->bghofs[n];
    }
    if ((offset >= 0x020 && offset <= 0x026) || (offset >= 0x030 && offset <= 0x036)) return *affine_param(ppu, offset);
    if ((offset >= 0x028 && offset <= 0x02E) || (offset >= 0x038 && offset <= 0x03E))
        return reference_read(ppu, offset);
    return 0;
}

void agb_ppu_write(AgbBoard* board, uint32_t offset, uint16_t value) {
    AgbPpu* ppu = &board->ppu;
    switch (offset) {
        case 0x000: ppu->dispcnt = value; return;
        case 0x002: ppu->greenswap = value; return;
        case 0x004: ppu->dispstat = (uint16_t)((ppu->dispstat & 0x0007) | (value & 0xFFF8)); return;
        case 0x006: return; /* VCOUNT is read only */
        case 0x040: ppu->win0h = value; return;
        case 0x042: ppu->win1h = value; return;
        case 0x044: ppu->win0v = value; return;
        case 0x046: ppu->win1v = value; return;
        case 0x048: ppu->winin = value; return;
        case 0x04A: ppu->winout = value; return;
        case 0x04C: ppu->mosaic = value; return;
        case 0x050: ppu->bldcnt = value; return;
        case 0x052: ppu->bldalpha = value & 0x1F1F; return;
        case 0x054: ppu->bldy = value & 0x1F; return;
        default: break;
    }
    if (offset >= 0x008 && offset <= 0x00E) {
        ppu->bgcnt[(offset - 0x008) >> 1] = value;
        return;
    }
    if (offset >= 0x010 && offset <= 0x01E) {
        uint32_t n = (offset - 0x010) >> 2;
        if (offset & 2)
            ppu->bgvofs[n] = value & 0x1FF;
        else
            ppu->bghofs[n] = value & 0x1FF;
        return;
    }
    if ((offset >= 0x020 && offset <= 0x026) || (offset >= 0x030 && offset <= 0x036)) {
        *affine_param(ppu, offset) = value;
        return;
    }
    if ((offset >= 0x028 && offset <= 0x02E) || (offset >= 0x038 && offset <= 0x03E))
        reference_write(ppu, offset, value);
}

/* ---- timing ---- */

void agb_ppu_step(AgbBoard* board, uint32_t cycles) {
    AgbPpu* ppu = &board->ppu;
    while (cycles) {
        uint32_t boundary = ppu->dot < 960 ? 960 : AGB_SCANLINE_CYCLES;
        uint32_t run = boundary - ppu->dot;
        if (run > cycles) run = cycles;
        ppu->dot += run;
        cycles -= run;
        if (ppu->dot != boundary) continue;
        if (boundary == 960) {
            /* the horizontal blank starts when the visible dots are over; its DMA only on the visible lines, as a game's
               table of one entry a line takes (the interrupt on every line) */
            ppu->dispstat |= 2;
            if (ppu->vcount < AGB_SCREEN_HEIGHT) {
                render_line(board, ppu->vcount);
                agb_dma_event(board, AGB_DMA_HBLANK);
            }
            if (ppu->dispstat & 0x10) agb_irq_request(board, AGB_IRQ_HBLANK);
        } else {
            ppu->dot = 0;
            ppu->dispstat &= (uint16_t)~2u;
            if (++ppu->vcount == AGB_FRAME_LINES) ppu->vcount = 0;
            if (ppu->vcount == AGB_SCREEN_HEIGHT) {
                /* the vertical blank starts: the reference points reload for the next frame, and a window's edge in
                   the blank's lines turns it as those lines would, as mGBA's FinishFrame has it */
                ppu->dispstat |= 1;
                for (int i = 0; i < 2; ++i) {
                    unsigned top = (i ? ppu->win1v : ppu->win0v) >> 8, bottom = (i ? ppu->win1v : ppu->win0v) & 0xFF;
                    if (bottom >= AGB_SCREEN_HEIGHT && bottom < AGB_FRAME_LINES) ppu->window_on[i] = 0;
                    if (top >= AGB_SCREEN_HEIGHT && top < AGB_FRAME_LINES && top > bottom) ppu->window_on[i] = 1;
                }
                for (int i = 2; i < 4; ++i) {
                    ppu->bgx_line[i] = ppu->bgx[i];
                    ppu->bgy_line[i] = ppu->bgy[i];
                }
                if (ppu->dispstat & 8) agb_irq_request(board, AGB_IRQ_VBLANK);
                agb_dma_event(board, AGB_DMA_VBLANK);
            }
            if (!ppu->vcount) ppu->dispstat &= (uint16_t)~1u;
            if (ppu->vcount == (ppu->dispstat >> 8)) {
                ppu->dispstat |= 4;
                if (ppu->dispstat & 0x20) agb_irq_request(board, AGB_IRQ_VCOUNT);
            } else
                ppu->dispstat &= (uint16_t)~4u;
        }
    }
}

/* ---- backgrounds ---- */

/* MOSAIC's block sizes, each its field + 1: the backgrounds' width at shift 0 and height at 4, the sprites' at 8 and
   12. A block of a layer that has mosaic on shows what its first pixel would, as mGBA draws it */
static int mosaic_size(const AgbPpu* ppu, int shift) { return (ppu->mosaic >> shift & 15) + 1; }

static void text_bg(AgbBoard* board, uint32_t index, int y) {
    AgbPpu* ppu = &board->ppu;
    uint16_t* out = scan.color[index];
    uint32_t cnt = ppu->bgcnt[index], charbase = (cnt >> 2 & 3) * 0x4000, screen = (cnt >> 8 & 0x1F) * 0x800;
    int bpp8 = cnt & 0x80, size = cnt >> 14, mosaic = cnt & 0x40;
    int width = size & 1 ? 512 : 256, height = size & 2 ? 512 : 256;
    int py, block = mosaic ? mosaic_size(ppu, 0) : 1;
    if (mosaic) y -= y % mosaic_size(ppu, 4); /* the line its block of lines starts at */
    py = (y + ppu->bgvofs[index]) & (height - 1);
    for (int x = 0; x < AGB_SCREEN_WIDTH; ++x) {
        int px = (x + ppu->bghofs[index]) & (width - 1);
        uint32_t block = ((uint32_t)(py >> 8) * (uint32_t)(width >> 8) + (uint32_t)(px >> 8)) * 0x800;
        uint32_t entry_address = screen + block + (uint32_t)((py >> 3) & 31) * 64 + (uint32_t)((px >> 3) & 31) * 2;
        uint16_t entry = vram16(board, entry_address);
        uint32_t tile = entry & 0x3FF;
        int tx = entry & 0x400 ? 7 - (px & 7) : px & 7, ty = entry & 0x800 ? 7 - (py & 7) : py & 7;
        uint32_t address;
        uint8_t color;
        if (bpp8) {
            address = charbase + tile * 64 + (uint32_t)ty * 8 + (uint32_t)tx;
            color = board->memory.vram[address & 0xFFFF];
        } else {
            address = charbase + tile * 32 + (uint32_t)ty * 4 + (uint32_t)tx / 2;
            uint8_t byte = board->memory.vram[address & 0xFFFF];
            color = tx & 1 ? byte >> 4 : byte & 15;
            if (color) color |= (uint8_t)(entry >> 12) << 4;
        }
        out[x] = color ? palette(board, color) : LAYER_TRANSPARENT;
    }
    for (int x = 0; block > 1 && x < AGB_SCREEN_WIDTH; ++x) out[x] = out[x - x % block];
}

/* The affine backgrounds walk the reference points their lines latched; y only places a line in its block of mosaic
   lines. A block's first pixel off a background that doesn't wrap leaves it open, and the next pixel starts the block */
static void affine_bg(AgbBoard* board, uint32_t index, int y) {
    AgbPpu* ppu = &board->ppu;
    uint16_t* out = scan.color[index];
    uint32_t cnt = ppu->bgcnt[index], charbase = (cnt >> 2 & 3) * 0x4000, screen = (cnt >> 8 & 0x1F) * 0x800;
    int wrap = cnt & 0x2000, pixels = 128 << (cnt >> 14), tiles = pixels / 8, block = 1, wait = 0;
    int64_t sx = ppu->bgx_line[index], sy = ppu->bgy_line[index];
    int32_t pa = (int16_t)ppu->bgpa[index], pc = (int16_t)ppu->bgpc[index];
    uint16_t held = LAYER_TRANSPARENT;
    if (cnt & 0x40) { /* the reference points of the line its block of lines starts at */
        int back = y % mosaic_size(ppu, 4);
        sx -= (int64_t)back * (int16_t)ppu->bgpb[index];
        sy -= (int64_t)back * (int16_t)ppu->bgpd[index];
        block = mosaic_size(ppu, 0);
    }
    for (int x = 0; x < AGB_SCREEN_WIDTH; ++x, sx += pa, sy += pc) {
        int32_t tx = (int32_t)(sx >> 8), ty = (int32_t)(sy >> 8);
        uint32_t tile, color;
        if (wait) {
            --wait;
            out[x] = held;
            continue;
        }
        if (wrap) {
            tx &= pixels - 1;
            ty &= pixels - 1;
        } else if (tx < 0 || ty < 0 || tx >= pixels || ty >= pixels) {
            out[x] = LAYER_TRANSPARENT;
            continue;
        }
        tile = board->memory.vram[screen + (uint32_t)(ty >> 3) * (uint32_t)tiles + (uint32_t)(tx >> 3)];
        color = board->memory.vram[charbase + tile * 64 + (uint32_t)(ty & 7) * 8 + (uint32_t)(tx & 7)];
        out[x] = held = color ? palette(board, color) : LAYER_TRANSPARENT;
        wait = block - 1;
    }
}

/* The bitmap modes draw into BG2's layer through BG2's rotation and scaling, walking its reference points as an affine
   background does, and use its priority, blend flags and mosaic. A pixel off the bitmap is open, and a mosaic block
   that starts there starts again on the next pixel, as mGBA draws them */
static void bitmap_bg(AgbBoard* board, int y, int mode) {
    AgbPpu* ppu = &board->ppu;
    uint16_t* out = scan.color[2];
    uint32_t page = ppu->dispcnt & 0x10 ? 0xA000 : 0;
    int32_t height = mode == 5 ? 128 : AGB_SCREEN_HEIGHT, width = mode == 5 ? 160 : AGB_SCREEN_WIDTH;
    int block = 1, wait = 0;
    int64_t sx = ppu->bgx_line[2], sy = ppu->bgy_line[2];
    int32_t pa = (int16_t)ppu->bgpa[2], pc = (int16_t)ppu->bgpc[2];
    uint16_t held = LAYER_TRANSPARENT;
    if (ppu->bgcnt[2] & 0x40) { /* the reference points of the line its block of lines starts at */
        int back = y % mosaic_size(ppu, 4);
        sx -= (int64_t)back * (int16_t)ppu->bgpb[2];
        sy -= (int64_t)back * (int16_t)ppu->bgpd[2];
        block = mosaic_size(ppu, 0);
    }
    for (int x = 0; x < AGB_SCREEN_WIDTH; ++x, sx += pa, sy += pc) {
        int32_t tx = (int32_t)(sx >> 8), ty = (int32_t)(sy >> 8);
        uint16_t color;
        if (wait) {
            --wait;
            out[x] = held;
            continue;
        }
        if (tx < 0 || ty < 0 || tx >= width || ty >= height) {
            out[x] = LAYER_TRANSPARENT;
            continue;
        }
        if (mode == 3)
            color = vram16(board, ((uint32_t)ty * 240 + (uint32_t)tx) * 2) & 0x7FFF;
        else if (mode == 4) {
            uint8_t index = board->memory.vram[page + (uint32_t)ty * 240 + (uint32_t)tx];
            color = index ? palette(board, index) : LAYER_TRANSPARENT;
        } else
            color = vram16(board, page + ((uint32_t)ty * 160 + (uint32_t)tx) * 2) & 0x7FFF;
        out[x] = held = color;
        wait = block - 1;
    }
}

/* ---- sprites ---- */

static const uint8_t obj_sizes[3][4][2] = {
    {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
    {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
    {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
};

static void obj_line(AgbBoard* board, int y) {
    AgbPpu* ppu = &board->ppu;
    int bitmap = (ppu->dispcnt & 7) >= 3, one_d = ppu->dispcnt & 0x40;
    int mosaic_w = mosaic_size(ppu, 8), mosaic_h = mosaic_size(ppu, 12);
    /* in OAM order, as the hardware: a sprite's pixel goes over one already drawn only with a lower priority value,
       so the lower index wins between sprites of one priority (a character over its shadow) */
    for (int i = 0; i < 128; ++i) {
        uint16_t a0 = oam16(board, (uint32_t)i * 8), a1 = oam16(board, (uint32_t)i * 8 + 2);
        uint16_t a2 = oam16(board, (uint32_t)i * 8 + 4);
        int affine = a0 & 0x100, objwin_mode = (a0 & 0xC00) == 0x800, semi = (a0 & 0xC00) == 0x400;
        int shape = a0 >> 14, size_index = a1 >> 14, width, height, draw_width, draw_height;
        int sx, rel, bpp8 = a0 & 0x2000, priority = a2 >> 10 & 3, tile = a2 & 0x3FF, mosaic = a0 & 0x1000, block, span;
        int32_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        /* a sprite that is not affine and has bit 9 is hidden (an affine one's bit 9 doubles its size) */
        if (shape == 3 || (a0 & 0xC00) == 0xC00 || (!affine && (a0 & 0x200))) continue;
        width = obj_sizes[shape][size_index][0];
        height = obj_sizes[shape][size_index][1];
        draw_width = width;
        draw_height = height;
        if (affine && (a0 & 0x200)) {
            draw_width = width * 2;
            draw_height = height * 2;
        }
        rel = (y - (a0 & 0xFF)) & 0xFF;
        if (rel >= draw_height) continue;
        sx = (int)(a1 & 0x1FF);
        if (sx >= 256) sx -= 512;
        /* mosaic: the row of the line its block of lines starts at, the sprite's first if that is above it, and blocks
           of pixels on the screen's grid, the last reaching past the sprite's right edge. A sprite of the OBJ window
           takes the rows alone, as in mGBA */
        if (mosaic) rel = rel < y % mosaic_h ? 0 : rel - y % mosaic_h;
        block = mosaic && !objwin_mode ? mosaic_w : 1;
        span = draw_width;
        if (block > 1 && sx + span > 0 && (sx + span) % block) span += block - (sx + span) % block;
        if (bitmap && tile < 512) continue;
        if (affine) {
            uint32_t group = a1 >> 9 & 31;
            pa = (int16_t)oam16(board, group * 32 + 6);
            pb = (int16_t)oam16(board, group * 32 + 14);
            pc = (int16_t)oam16(board, group * 32 + 22);
            pd = (int16_t)oam16(board, group * 32 + 30);
        }
        for (int px = 0; px < span; ++px) {
            int screen_x = sx + px, tx, ty, from = px;
            uint32_t units, address;
            uint8_t color;
            if (screen_x < 0 || screen_x >= AGB_SCREEN_WIDTH) continue;
            if (affine) {
                /* the pixel its block starts at, or the one before the sprite when the block starts left of it */
                int64_t cx, cy = rel - draw_height / 2;
                if (block > 1) {
                    int start = screen_x - screen_x % block;
                    from = start < sx ? -1 : start - sx;
                }
                cx = from - draw_width / 2;
                tx = (int)((pa * cx + pb * cy) >> 8) + width / 2;
                ty = (int)((pc * cx + pd * cy) >> 8) + height / 2;
                if (tx < 0 || ty < 0 || tx >= width || ty >= height) continue;
            } else {
                /* the column its block starts at, held to the sprite's own */
                if (block > 1) {
                    from = px - screen_x % block;
                    from = from < 0 ? 0 : from > width - 1 ? width - 1 : from;
                }
                tx = a1 & 0x1000 ? width - 1 - from : from;
                ty = a1 & 0x2000 ? height - 1 - rel : rel;
            }
            /* tiles are 32 byte units; 8bpp sprites take two of them per tile */
            if (one_d) {
                units = (uint32_t)tile + (uint32_t)(ty >> 3) * (uint32_t)(bpp8 ? width / 4 : width / 8) +
                        (uint32_t)(tx >> 3) * (bpp8 ? 2u : 1u);
            } else {
                /* the two dimensional mapping holds 32 units per tile row */
                uint32_t base = bpp8 ? (uint32_t)tile & ~(uint32_t)1 : (uint32_t)tile;
                uint32_t in_row = (base & 31) + (uint32_t)(tx >> 3) * (bpp8 ? 2u : 1u);
                units = (base & ~(uint32_t)31) + (uint32_t)(ty >> 3) * 32 + (in_row & 31);
            }
            address = units * 32;
            if (bpp8)
                color =
                    board->memory.vram[0x10000 + ((address + (uint32_t)(ty & 7) * 8 + (uint32_t)(tx & 7)) & 0x7FFF)];
            else {
                uint8_t byte =
                    board->memory
                        .vram[0x10000 + ((address + (uint32_t)(ty & 7) * 4 + (uint32_t)(tx & 7) / 2) & 0x7FFF)];
                color = tx & 1 ? byte >> 4 : byte & 15;
                if (color) color |= (uint8_t)(a2 >> 12) << 4;
            }
            if (!color) {
                /* an open pixel still gives its priority, and whether it is semi transparent, to the pixel a sprite
                   of a lower one drew there, as mGBA draws it: that pixel goes over the backgrounds this one would */
                if (scan.drawn[screen_x] && priority < scan.obj_pri[screen_x]) {
                    scan.obj_pri[screen_x] = (uint8_t)priority;
                    scan.obj_mode[screen_x] = (uint8_t)(semi ? 1 : 0);
                }
                continue;
            }
            if (objwin_mode) {
                scan.objwin[screen_x] = 1;
                continue;
            }
            if (scan.drawn[screen_x] && priority >= scan.obj_pri[screen_x]) continue;
            scan.drawn[screen_x] = 1;
            scan.color[4][screen_x] = palette(board, 0x100 + color);
            scan.obj_pri[screen_x] = (uint8_t)priority;
            scan.obj_mode[screen_x] = (uint8_t)(semi ? 1 : 0);
        }
    }
}

/* ---- composition ---- */

/* WIN0's and WIN1's lines as the line counter meets their top and bottom: a window turns on at its top's line and off
   at its bottom's, and stays as it is in between, across the frame's end too (mGBA's StepWindow) */
static void window_step(AgbPpu* ppu, int y) {
    for (int i = 0; i < 2; ++i) {
        uint16_t edges = i ? ppu->win1v : ppu->win0v;
        if (y == edges >> 8) ppu->window_on[i] = 1;
        if (y == (edges & 0xFF)) ppu->window_on[i] = 0;
    }
}

static uint8_t window_mask(const AgbPpu* ppu, int x, int objwin) {
    if (!(ppu->dispcnt & 0xE000)) return 0x3F;
    if ((ppu->dispcnt & 0x2000) && ppu->window_on[0]) {
        uint8_t x1 = ppu->win0h >> 8, x2 = ppu->win0h;
        if (x1 <= x2 ? x >= x1 && x < x2 : x >= x1 || x < x2) return ppu->winin & 0x3F;
    }
    if ((ppu->dispcnt & 0x4000) && ppu->window_on[1]) {
        uint8_t x1 = ppu->win1h >> 8, x2 = ppu->win1h;
        if (x1 <= x2 ? x >= x1 && x < x2 : x >= x1 || x < x2) return (ppu->winin >> 8) & 0x3F;
    }
    if ((ppu->dispcnt & 0x8000) && objwin) return (ppu->winout >> 8) & 0x3F;
    return ppu->winout & 0x3F;
}

/* a blend weight of BLDALPHA's or BLDY's, its five bits held to 16 sixteenths, as the hardware and mGBA hold them */
static int32_t weight(uint32_t field) { return (field & 0x1F) > 16 ? 16 : (int32_t)(field & 0x1F); }

static void composite_line(AgbBoard* board, int y) {
    AgbPpu* ppu = &board->ppu;
    uint32_t* out = ppu->framebuffer + y * AGB_SCREEN_WIDTH;
    int forced_blank = ppu->dispcnt & 0x80;
    /* a second target among the layers shown, the backdrop always: what a semi transparent sprite's light needs */
    int seconds = ppu->bldcnt >> 13 & 1;
    for (int i = 0; i < 4; ++i) seconds |= (ppu->dispcnt >> (8 + i) & 1) && (ppu->bldcnt >> (8 + i) & 1);
    for (int x = 0; x < AGB_SCREEN_WIDTH; ++x) {
        int top = -1, second = -1, top_key = 99, second_key = 99;
        uint8_t mask = window_mask(ppu, x, scan.objwin[x]);
        uint16_t color;
        int effect = ppu->bldcnt >> 6 & 3;
        if (forced_blank) {
            out[x] = 0xFFu << 24 | 0xFFFFFFu;
            continue;
        }
        for (int layer = 4; layer >= 0; --layer) {
            int key;
            uint16_t value = scan.color[layer][x];
            if (value & LAYER_TRANSPARENT) continue;
            if (layer == 4) {
                if (!(mask & 0x10)) continue;
                key = scan.obj_pri[x] * 8;
            } else {
                if (!(mask & (1 << layer))) continue;
                key = (ppu->bgcnt[layer] & 3) * 8 + 1 + layer;
            }
            if (key < top_key) {
                second = top;
                second_key = top_key;
                top = layer;
                top_key = key;
            } else if (key < second_key) {
                second = layer;
                second_key = key;
            }
        }
        /* the backdrop fills the pixel or stands under everything else; under the backdrop is nothing to blend with */
        if (top < 0)
            top = 5;
        else if (second < 0)
            second = 5;
        {
            uint16_t first_color = top == 5 ? palette(board, 0) : scan.color[top][x];
            uint16_t second_color = second < 0 ? 0 : second == 5 ? palette(board, 0) : scan.color[second][x];
            int first_target = ppu->bldcnt >> top & 1;
            int second_target = second >= 0 && (ppu->bldcnt >> (second + 8) & 1);
            int semi = top == 4 && scan.obj_mode[x] == 1;
            /* the brightness effect lights the first targets, and a semi transparent sprite that has nothing to blend
               with below when OBJ is a first target or another layer a second one, as mGBA's reblend has it */
            int lit = (mask & 0x20) && (effect == 2 || effect == 3) && (first_target || (semi && seconds));
            color = first_color;
            if (second_target && (semi || ((mask & 0x20) && effect == 1 && first_target))) {
                /* alpha blend: a semi transparent sprite blends whether or not the effect selects it, or the window
                   lets effects through, as mGBA has it */
                int32_t eva = weight(ppu->bldalpha), evb = weight(ppu->bldalpha >> 8);
                uint32_t result = 0;
                for (int component = 0; component < 3; ++component) {
                    int32_t c1 = first_color >> (component * 5) & 31, c2 = second_color >> (component * 5) & 31;
                    int32_t mixed = (c1 * eva + c2 * evb) >> 4;
                    result |= (uint32_t)(mixed > 31 ? 31 : mixed) << (component * 5);
                }
                color = (uint16_t)result;
            } else if (lit) {
                int32_t evy = weight(ppu->bldy);
                uint32_t result = 0;
                for (int component = 0; component < 3; ++component) {
                    int32_t c1 = first_color >> (component * 5) & 31;
                    int32_t mixed = effect == 2 ? c1 + (((31 - c1) * evy) >> 4) : c1 - ((c1 * evy) >> 4);
                    result |= (uint32_t)(mixed > 31 ? 31 : mixed) << (component * 5);
                }
                color = (uint16_t)result;
            }
        }
        out[x] = rgb888(color);
    }
}

static void render_line(AgbBoard* board, int y) {
    AgbPpu* ppu = &board->ppu;
    int mode = ppu->dispcnt & 7;
    window_step(ppu, y);
    for (int layer = 0; layer < LAYERS; ++layer)
        for (int x = 0; x < AGB_SCREEN_WIDTH; ++x) scan.color[layer][x] = LAYER_TRANSPARENT;
    memset(scan.obj_pri, 0, sizeof(scan.obj_pri));
    memset(scan.obj_mode, 0, sizeof(scan.obj_mode));
    memset(scan.objwin, 0, sizeof(scan.objwin));
    memset(scan.drawn, 0, sizeof(scan.drawn));
    if (!(ppu->dispcnt & 0x80)) {
        if (mode == 0) {
            for (int i = 0; i < 4; ++i)
                if (ppu->dispcnt & (0x100 << i)) text_bg(board, (uint32_t)i, y);
        } else if (mode == 1) {
            if (ppu->dispcnt & 0x100) text_bg(board, 0, y);
            if (ppu->dispcnt & 0x200) text_bg(board, 1, y);
            if (ppu->dispcnt & 0x400) affine_bg(board, 2, y);
        } else if (mode == 2) {
            if (ppu->dispcnt & 0x400) affine_bg(board, 2, y);
            if (ppu->dispcnt & 0x800) affine_bg(board, 3, y);
        } else if (ppu->dispcnt & 0x400)
            bitmap_bg(board, y, mode);
        if (ppu->dispcnt & 0x1000) obj_line(board, y);
    }
    composite_line(board, y);
    for (int i = 2; i < 4; ++i) {
        ppu->bgx_line[i] += (int16_t)ppu->bgpb[i];
        ppu->bgy_line[i] += (int16_t)ppu->bgpd[i];
    }
}
