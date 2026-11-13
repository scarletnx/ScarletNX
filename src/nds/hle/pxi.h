/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NDS_PXI_H
#define SNX_NDS_PXI_H
#include "nds/board/nds.h"
/* The sound driver the SDK's ARM7 program runs (hle/sound.c): its commands come over the FIFO, and it plays through the
   sixteen channels of the sound hardware (audio/spu.c) */
bool nds_sound_init(NdsBoard* board);
void nds_sound_free(NdsBoard* board);
void nds_sound_request(NdsBoard* board, uint32_t data);
/* the driver's turn, every 5.2 ms of the board's time */
void nds_sound_tick(NdsBoard* board);
/* an alarm the game set is due (NDS_EVENT_ALARM) */
void nds_sound_alarm(NdsBoard* board);
#endif
