/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
#include "agb/board/board.h"
#include <stdio.h>
#include <string.h>
#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);                                           \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

static AgbBoard board;
static int16_t out[2 * 20480];

/* Every section starts from a silent board with the master switch on. */
static void reset(void) {
    agb_board_destroy(&board);
    agb_board_init(&board);
    agb_apu_write(&board, 0x084, 0x0080);
}

/* One stereo pair of samples per 512 cycles. */
static uint32_t run_samples(uint32_t count) {
    CHECK(count * 2 <= sizeof(out) / sizeof(out[0]));
    memset(out, 0, sizeof(out));
    return agb_apu_run(&board, count * 512, out, count);
}

/* the same with the timers running beside the sound, in steps of 64 cycles as the machine runs them */
static uint32_t run_timed(uint32_t count) {
    uint32_t written = 0;
    CHECK(count * 2 <= sizeof(out) / sizeof(out[0]));
    memset(out, 0, sizeof(out));
    for (uint32_t cycles = 0; cycles < count * 512; cycles += 64) {
        agb_timer_step(&board, 64);
        written += agb_apu_run(&board, 64, out + written * 2, count - written);
    }
    return written;
}

/* how many of the first count left samples are value */
static int left_count(uint32_t count, int16_t value) {
    int n = 0;
    for (uint32_t i = 0; i < count; ++i) n += out[i * 2] == value;
    return n;
}

int main(void) {
    /* square 2: 50 percent duty at 128 Hz, envelope volume 15 held constant */
    reset();
    agb_apu_write(&board, 0x080, 0x2277); /* both master volumes 7, square 2 to both sides */
    agb_apu_write(&board, 0x082, 0x0002); /* PSG at 100 percent */
    agb_apu_write(&board, 0x068, 0xF080);
    agb_apu_write(&board, 0x06C, 0x8400); /* frequency 1024 and trigger */
    CHECK(run_samples(256) == 256);
    CHECK(out[0] == 960 && out[1] == 960); /* 15 at full volume, scaled to 16 bits */
    CHECK(out[2 * 127] == 960);
    CHECK(out[2 * 128] == -960 && out[2 * 200] == -960); /* the duty cycle's second half */

    /* a volume's bits 15 and 14 neither trigger nor turn the length on: the frequency register's do */
    reset();
    agb_apu_write(&board, 0x068, 0xF080); /* square 2 at volume 15 */
    agb_apu_write(&board, 0x072, 0x6000); /* the wave at 25 percent */
    agb_apu_write(&board, 0x078, 0xF000); /* the noise at volume 15 */
    CHECK(!board.apu.enabled[1] && !board.apu.length_enabled[1]);
    CHECK(!board.apu.length_enabled[2] && !board.apu.length_enabled[3]);
    agb_apu_write(&board, 0x06C, 0xC400);
    CHECK(board.apu.enabled[1] && board.apu.length_enabled[1]);

    /* the noise register steps at 524288 Hz / r / 2^(s+1), r 0 a half: 8 steps a sample at r 1, s 0, and 16 at r 0 */
    for (uint32_t r = 0; r < 2; ++r) {
        uint16_t lfsr = 0x7FFF;
        reset();
        agb_apu_write(&board, 0x080, 0x8877); /* the noise to both sides */
        agb_apu_write(&board, 0x082, 0x0002);
        agb_apu_write(&board, 0x078, 0xF000);
        agb_apu_write(&board, 0x07C, (uint16_t)(0x8000 | r)); /* 15 bit register, s 0, trigger */
        CHECK(run_samples(16) == 16);
        for (int step = 0; step < (r ? 8 : 16) * 16; ++step)
            lfsr = (uint16_t)(lfsr >> 1 | ((lfsr ^ lfsr >> 1) & 1) << 14);
        CHECK(board.apu.noise_lfsr == lfsr);
    }

    /* the envelope steps down at 64 Hz */
    reset();
    agb_apu_write(&board, 0x080, 0x2277);
    agb_apu_write(&board, 0x082, 0x0002);
    agb_apu_write(&board, 0x068, 0xF180); /* direction down, period 1 */
    agb_apu_write(&board, 0x06C, 0x8400);
    CHECK(run_samples(600) == 600);
    CHECK(out[2 * 300] == 960); /* still 15 before the first step */
    CHECK(out[2 * 512] == 896); /* volume 14 after it */

    /* the length counter stops the channel */
    reset();
    agb_apu_write(&board, 0x080, 0x2277);
    agb_apu_write(&board, 0x082, 0x0002);
    agb_apu_write(&board, 0x068, 0xF0A0); /* length load 32 */
    agb_apu_write(&board, 0x06C, 0xC400); /* frequency 1024, length on, trigger */
    CHECK(run_samples(4200) == 4200);
    CHECK(out[2 * 3900] == 960);
    CHECK(out[2 * 4100] == 0); /* the counter ran out at 32 * 128 samples */

    /* the wave channel plays its 4 bit digits */
    reset();
    agb_apu_write(&board, 0x070, 0x40); /* bank 1 to play: the registers reach bank 0 */
    for (int i = 0; i < 8; ++i) agb_apu_write(&board, 0x090 + (uint32_t)i * 2, 0xFFFF);
    agb_apu_write(&board, 0x080, 0x4477); /* wave to both sides */
    agb_apu_write(&board, 0x082, 0x0002); /* PSG at 100 percent */
    agb_apu_write(&board, 0x070, 0x80);   /* playing */
    agb_apu_write(&board, 0x072, 0x2000); /* full volume */
    agb_apu_write(&board, 0x074, 0x8400); /* frequency 1024 and trigger */
    CHECK(run_samples(64) == 64);
    CHECK(out[0] == 960 && out[2 * 63] == 960); /* every digit is 15 */

    /* a byte's high digit plays first; the registers reach the bank the channel does not play, while it plays; with
       bit 5 it plays 64 digits, its bank's then the other's; bit 15 forces 75 percent. At frequency 1984 a digit lasts
       one output sample */
    reset();
    agb_apu_write(&board, 0x080, 0x4477);
    agb_apu_write(&board, 0x082, 0x0002);
    agb_apu_write(&board, 0x070, 0x40);   /* the registers reach bank 0 */
    agb_apu_write(&board, 0x090, 0x00F0); /* digits 15, 0, then 0, 0 */
    agb_apu_write(&board, 0x070, 0x80);   /* bank 0 plays: the registers reach bank 1 */
    for (int i = 0; i < 8; ++i) agb_apu_write(&board, 0x090 + (uint32_t)i * 2, 0xFFFF);
    agb_apu_write(&board, 0x072, 0x2000);
    agb_apu_write(&board, 0x074, 0x8000 | 1984);
    CHECK(run_samples(32) == 32);
    CHECK(out[0] == 960 && out[2 * 1] == -960 && out[2 * 2] == -960 && out[2 * 31] == -960);
    agb_apu_write(&board, 0x070, 0xC0); /* bank 1 plays, written while bank 0 played */
    CHECK(run_samples(4) == 4 && left_count(4, 960) == 4);
    agb_apu_write(&board, 0x070, 0xA0); /* 64 digits from bank 0 */
    agb_apu_write(&board, 0x074, 0x8000 | 1984);
    CHECK(run_samples(64) == 64);
    CHECK(out[0] == 960 && out[2 * 1] == -960 && out[2 * 31] == -960 && left_count(64, 960) == 33);
    agb_apu_write(&board, 0x072, 0x8000); /* 75 percent */
    agb_apu_write(&board, 0x070, 0xC0);
    CHECK(run_samples(4) == 4 && left_count(4, 11 * 64) == 4);

    /* the FIFOs play the bytes the direct sound DMA queues, one each time their timer overflows, held between: none
       while no timer runs, and with timer 0 at 1024 cycles each byte lasts two output samples of 512 */
    reset();
    agb_apu_write(&board, 0x082, 0x0304); /* FIFO A at full volume to both sides, on timer 0 */
    agb_apu_write(&board, 0x0A0, 0x9C64); /* the bytes 100 and -100 */
    CHECK(run_samples(4) == 4 && left_count(4, 0) == 4 && board.apu.fifo_count[0] == 2);
    agb_timer_write(&board, 0x100, 0xFC00); /* TM0 reloads 1024 below its overflow */
    agb_timer_write(&board, 0x102, 0x0080);
    CHECK(run_timed(8) == 8);
    CHECK(left_count(8, 6400) == 2 && left_count(8, -6400) == 2 && out[1] == out[0]);
    CHECK(board.apu.fifo_count[0] == 0); /* both bytes played, then the empty queue's 0 */

    /* FIFO B at half volume to both sides (bits 12 and 13), on timer 0 too; its left bit leaves FIFO A's queue alone
       (bit 11 is FIFO A's reset) */
    agb_apu_write(&board, 0x0A0, 0x1010);
    agb_apu_write(&board, 0x082, 0x3000);
    CHECK(board.apu.fifo_count[0] == 2);
    agb_apu_write(&board, 0x0A4, 100);
    CHECK(run_timed(4) == 4 && left_count(4, 3200) >= 1 && out[1] == out[0]);
    agb_apu_write(&board, 0x082, 0x0800); /* FIFO A's reset */
    CHECK(board.apu.fifo_count[0] == 0);

    /* panning: a channel sent to one side leaves the other silent */
    reset();
    agb_apu_write(&board, 0x082, 0x0002);
    agb_apu_write(&board, 0x080, 0x027F); /* master volumes 7 and 7, square 2 right only */
    agb_apu_write(&board, 0x068, 0xF080);
    agb_apu_write(&board, 0x06C, 0x8400);
    CHECK(run_samples(8) == 8);
    CHECK(out[1] == 960 && out[0] == 0);

    /* the master switch silences everything */
    agb_apu_write(&board, 0x084, 0x0000);
    CHECK(run_samples(8) == 8);
    CHECK(out[0] == 0 && out[1] == 0);

    puts("agb_apu: ok");
    return 0;
}
