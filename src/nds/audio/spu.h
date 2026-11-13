/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The sound hardware: sixteen channels (PCM, ADPCM, the six square waves and the two noise channels) mixed to stereo
   at the console's own rate, a sample every 1024 bus cycles (32728.5 Hz). Its registers are the ARM7's, so the game
   never writes them: the sound driver the runtime runs in the ARM7's place does (hle/sound.c). */
#ifndef SNX_NDS_SPU_H
#define SNX_NDS_SPU_H
#include "nds/board/nds.h"
#define NDS_SPU_SAMPLE_CYCLES 1024u
#define NDS_SPU_RATE          ((double)NDS_CLOCK / NDS_SPU_SAMPLE_CYCLES)
bool nds_spu_init(NdsBoard* board);
void nds_spu_free(NdsBoard* board);
/* the registers at 0x04000400 to 0x0400051F */
uint32_t nds_spu_read(NdsBoard* board, uint32_t address, unsigned bytes);
void nds_spu_write(NdsBoard* board, uint32_t address, unsigned bytes, uint32_t value);
/* the samples up to the board's time now, mixed and kept for the host */
void nds_spu_run(NdsBoard* board);
/* up to most frames of them (left and right, 16 bits each) into out: how many */
uint32_t nds_spu_take(NdsBoard* board, int16_t* out, uint32_t most);
#endif
