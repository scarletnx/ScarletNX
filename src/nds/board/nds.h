/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The NDS board as its main processor sees it. The ARM9's program is the game, compiled ahead of time; nothing else of
   the console is run as code. The second processor (the ARM7, whose program is the SDK's own: sound, the touch
   screen, the clock, the card's save memory, the firmware's settings) is not emulated: what the game asks of it over
   the inter-processor FIFO is answered here (hle/pxi.c), as the 3DS's services answer its IPC. The BIOS's calls are
   board routines (bios/bios.c). The display's two 2D engines and the 3D engine are drawn on the GPU (gpu/). */
#ifndef SNX_NDS_H
#define SNX_NDS_H
#include "common/memory/host.h"
#include "nds/cartridge/rom.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NDS_SCREEN_WIDTH  256u
#define NDS_SCREEN_HEIGHT 192u
/* the bus clock, 33.513982 MHz: a dot is 6 of its cycles, a line 355 dots, a frame 263 lines (59.8261 Hz) */
#define NDS_CLOCK         33513982u
#define NDS_LINE_CYCLES   2130u
#define NDS_HBLANK_CYCLE  (256u * 6u + 48u)
#define NDS_FRAME_LINES   263u
#define NDS_FRAME_CYCLES  (NDS_LINE_CYCLES * NDS_FRAME_LINES)

#define NDS_MAIN_RAM_SIZE 0x400000u
#define NDS_WRAM_SIZE     0x8000u
#define NDS_ITCM_SIZE     0x8000u
#define NDS_DTCM_SIZE     0x4000u
#define NDS_MAP_SIZE      0x100000000ull
#define NDS_MAP_GUARD     0x10000u

/* the ARM9's interrupt lines, IE's and IF's bits */
enum {
    NDS_IRQ_VBLANK = 0,
    NDS_IRQ_HBLANK = 1,
    NDS_IRQ_VCOUNT = 2,
    NDS_IRQ_TIMER0 = 3,
    NDS_IRQ_DMA0 = 8,
    NDS_IRQ_KEYPAD = 12,
    NDS_IRQ_IPC_SYNC = 16,
    NDS_IRQ_IPC_SEND_DONE = 17,
    NDS_IRQ_IPC_RECV = 18,
    NDS_IRQ_CARD_DONE = 19,
    NDS_IRQ_CARD_IREQ = 20,
    NDS_IRQ_GXFIFO = 21
};

/* the nine VRAM banks, A to I */
enum { NDS_VRAM_BANKS = 9 };
extern const uint32_t nds_vram_bank_size[NDS_VRAM_BANKS];

/* What the board's time is spent waiting for, each due at a cycle (UINT64_MAX: not waiting) */
typedef enum {
    NDS_EVENT_LCD,     /* the line's horizontal blank, then its end */
    NDS_EVENT_TIMER0,  /* a timer's overflow, four of them */
    NDS_EVENT_SOUND = NDS_EVENT_TIMER0 + 4, /* the sound driver's tick (hle/sound.c) */
    NDS_EVENT_ALARM,                        /* the first of its alarms due */
    NDS_EVENT_COUNT
} NdsEvent;

typedef struct {
    uint16_t reload, control;
    uint32_t counter;  /* the count at since, for a timer that counts the one below it the count itself */
    uint64_t since;    /* the cycle counter was read at */
} NdsTimer;

typedef struct {
    uint32_t source, destination, control;
    uint32_t at_source, at_destination, left; /* the transfer under way */
    uint8_t running;
} NdsDma;

typedef struct {
    unsigned char* base; /* the flat window the compiled blocks run on: guest address + base */
    SnxHostMemory main, wram, itcm, dtcm, palette, oam, scratch;
    SnxHostMemory vram[NDS_VRAM_BANKS];
    uint32_t dtcm_base, dtcm_size; /* where the DTCM is in the window now, 0 bytes when it is nowhere */
    size_t page;
    /* each VRAM space's pages (nds_vram_copy): the bank in bits 24 up and the offset in it, NDS_VRAM_NONE for no bank.
       The first five spaces are the ARM9's own addresses, and seen says what the window shows at each page now */
    uint32_t space[11][41], seen[5][41];
} NdsMemory;
#define NDS_VRAM_NONE 0xFFFFFFFFu

/* The 2D engines' registers as one line was drawn with them, which the GPU composes the frame from (gpu/vk): the
   registers a game changes between lines (a horizontal blank's scrolls, windows and blends) take effect on the line
   they would on the console */
#define NDS_LINE_WORDS 32u
typedef struct {
    uint32_t words[NDS_LINE_WORDS];
} NdsLine;

typedef struct NdsBoard NdsBoard;
struct NdsGpu3D;
struct NdsSound;

struct NdsBoard {
    NdsMemory mem;
    NdsRom* rom;
    uint64_t cycles; /* bus cycles since the board started */
    uint64_t events[NDS_EVENT_COUNT], next_event;
    /* the ARM9's interrupts */
    uint32_t ime, ie, irf;
    uint8_t halted;      /* waiting for an interrupt (the BIOS's Halt, CP15's wait) */
    uint8_t postflg;
    /* the display's timing */
    uint16_t dispstat, vcount;
    uint8_t in_hblank, frame_done;
    uint64_t line_start; /* the cycle the line began at */
    uint64_t frames;
    uint16_t powcnt;
    /* the registers of 0x04000000 to 0x0400006F and 0x04001000 to 0x0400106F as written, by halfword */
    uint16_t lcd[2][0x38];
    int32_t affine_x[2][2], affine_y[2][2]; /* BG2's and BG3's reference points as the line counts them on */
    uint8_t window_y[2][2];                 /* each window's vertical flag: inside its lines */
    /* the main memory display: the frame's pixels as a DMA channel feeds them to DISP_MMEM_FIFO, a line of 256 at each
       visible line, and where the next two go */
    uint16_t main_display[NDS_SCREEN_HEIGHT * NDS_SCREEN_WIDTH];
    uint32_t main_display_at;
    NdsLine lines[2][NDS_SCREEN_HEIGHT];
    uint8_t vramcnt[NDS_VRAM_BANKS], wramcnt;
    uint16_t exmemcnt;
    NdsDma dma[4];
    uint32_t dma_fill[4];
    NdsTimer timer[4];
    /* the divider and the square root */
    uint16_t divcnt, sqrtcnt;
    uint64_t div_numerator, div_denominator, div_quotient, div_remainder, sqrt_value;
    uint32_t sqrt_result;
    /* the keypad: KEYINPUT's ten keys (a 0 bit is a key held), and what the ARM7 reads for the game: X, Y, the pen
       and the hinge */
    uint16_t keyinput, keycnt, extkeyin;
    uint16_t touch_x, touch_y; /* the pen on the lower screen, in its pixels */
    uint8_t touching;
    /* the inter-processor port */
    uint16_t ipcsync, ipcfifocnt;
    uint32_t ipc_recv[64]; /* what the ARM7 sent and the game has not read: the FIFO's 16 words, then the ones waiting */
    unsigned ipc_recv_at, ipc_recv_count;
    uint32_t ipc_last; /* the word a read of the empty FIFO gives again */
    struct NdsPxi* pxi;
    /* the card */
    uint16_t auxspicnt;
    uint32_t romctrl;
    uint8_t card_command[8];
    uint32_t card_at, card_left, card_address; /* a transfer under way: the next word's place, the bytes left */
    uint8_t card_kind;                         /* what it reads: 0 nothing (0xFF), 1 the image, 2 the chip ID */
    unsigned char* save;                       /* the card's save memory */
    uint32_t save_size;
    uint8_t save_dirty;
    /* CP15 */
    uint32_t cp15_control, cp15_dtcm, cp15_itcm;
    uint32_t cp15_regions[8], cp15_misc[8];
    struct NdsGpu3D* gpu3d;
    struct NdsSound* sound;
    void* spu; /* the sound hardware (audio/spu.c) */
    /* what the board says once of what it does not do */
    uint32_t told[8];
    int trace;
};

/* ---- board.c ---- */
/* The board with rom in its slot and the state the firmware leaves a game it started: the header and the program in
   main memory, the user's settings, the registers. False with the reason in error */
bool nds_board_init(NdsBoard* board, NdsRom* rom, char* error, size_t error_size);
void nds_board_free(NdsBoard* board);
void nds_irq(NdsBoard* board, unsigned line);
static inline bool nds_irq_pending(const NdsBoard* board) { return (board->ime & 1) && (board->ie & board->irf); }
void nds_schedule(NdsBoard* board, NdsEvent event, uint64_t at);
/* everything due by now, in time's order */
void nds_run_events(NdsBoard* board);
/* a line the board says once: the first time key is seen */
bool nds_once(NdsBoard* board, unsigned key);

/* ---- memory.c ---- */
bool nds_memory_init(NdsBoard* board, char* error, size_t error_size);
void nds_memory_free(NdsBoard* board);
/* the banks at the addresses VRAMCNT_A to VRAMCNT_I give them, after a write to one */
void nds_vram_remap(NdsBoard* board);
/* the DTCM where CP15's register says */
void nds_dtcm_remap(NdsBoard* board);
/* Memory as a device or a BIOS routine reads and writes it: the window, and the registers at 0x04000000 */
uint8_t nds_read8(NdsBoard* board, uint32_t address);
uint16_t nds_read16(NdsBoard* board, uint32_t address);
uint32_t nds_read32(NdsBoard* board, uint32_t address);
void nds_write8(NdsBoard* board, uint32_t address, uint8_t value);
void nds_write16(NdsBoard* board, uint32_t address, uint16_t value);
void nds_write32(NdsBoard* board, uint32_t address, uint32_t value);
/* The VRAM as the 2D engines and the 3D engine address it, for the GPU's copy: each space's 16 KiB pages (8 KiB for
   the extended palettes), the bank's bytes for a page that has one, NULL for none */
typedef enum {
    NDS_SPACE_BG_A,
    NDS_SPACE_BG_B,
    NDS_SPACE_OBJ_A,
    NDS_SPACE_OBJ_B,
    NDS_SPACE_LCDC,
    NDS_SPACE_BG_EXTPAL_A,
    NDS_SPACE_BG_EXTPAL_B,
    NDS_SPACE_OBJ_EXTPAL_A,
    NDS_SPACE_OBJ_EXTPAL_B,
    NDS_SPACE_TEXTURE,
    NDS_SPACE_TEXPAL,
    NDS_SPACE_COUNT
} NdsVramSpace;
extern const uint32_t nds_space_size[NDS_SPACE_COUNT];
/* the space's bytes copied to out (nds_space_size of them), zeros where no bank is */
void nds_vram_copy(const NdsBoard* board, NdsVramSpace space, unsigned char* out);

/* ---- io.c ---- */
uint32_t nds_io_read(NdsBoard* board, uint32_t address, unsigned bytes);
void nds_io_write(NdsBoard* board, uint32_t address, unsigned bytes, uint32_t value);

/* ---- lcd.c ---- */
void nds_lcd_reset(NdsBoard* board);
void nds_lcd_event(NdsBoard* board);
uint16_t nds_lcd_read(NdsBoard* board, unsigned engine, uint32_t offset);
void nds_lcd_write(NdsBoard* board, unsigned engine, uint32_t offset, uint16_t value, uint16_t mask);

/* ---- dma.c ---- */
enum { NDS_DMA_NOW, NDS_DMA_VBLANK, NDS_DMA_HBLANK, NDS_DMA_LINE, NDS_DMA_DISPLAY, NDS_DMA_CARD, NDS_DMA_PAK, NDS_DMA_GXFIFO };
void nds_dma_write_control(NdsBoard* board, unsigned channel, uint32_t value);
/* every channel started by when runs */
void nds_dma_trigger(NdsBoard* board, unsigned when);

/* ---- timers.c ---- */
uint16_t nds_timer_read(NdsBoard* board, unsigned index);
void nds_timer_write(NdsBoard* board, unsigned index, uint16_t reload, uint16_t control, unsigned which);
void nds_timer_event(NdsBoard* board, unsigned index);

/* ---- card.c ---- */
void nds_card_write_romctrl(NdsBoard* board, uint32_t value);
uint32_t nds_card_read_data(NdsBoard* board);

/* ---- hle/pxi.c ---- */
bool nds_pxi_init(NdsBoard* board);
void nds_pxi_free(NdsBoard* board);
/* the game wrote IPCSYNC, or a word to the FIFO */
void nds_pxi_sync(NdsBoard* board);
void nds_pxi_request(NdsBoard* board, uint32_t word);
/* a line of the display began: what the ARM7 does by the line (the touch screen's samples, the keys) */
void nds_pxi_line(NdsBoard* board, unsigned line);
/* the sound driver's tick is due (NDS_EVENT_SOUND) */
void nds_pxi_tick(NdsBoard* board);
/* a word from the ARM7 into the FIFO the game reads */
void nds_ipc_reply(NdsBoard* board, uint32_t word);
#endif
