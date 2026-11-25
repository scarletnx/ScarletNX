/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A DS game's save states (state.c): the board, the ARM9, the console's memory, the 3D engine, the sound hardware and
   the ARM7's side with its sound driver in a state file, the Switch's layout (nx/runtime/state_file.h), so a state's
   header and thumbnail read the same on every console. The runner calls both between two frames, on the thread the
   machine runs on. The card's save memory is the game's save file (<name>.sav), which a state never takes back, as a
   Switch game's save files */
#ifndef SNX_NDS_STATE_H
#define SNX_NDS_STATE_H
#include "nds/board/nds.h"
#include "nds/cpu/nds_cpu.h"
#include <stddef.h>
#include <stdint.h>

/* what a DS state holds and how, in its header's abi field: a state of another version is refused, naming both */
#define SNX_NDS_STATE_VERSION 1

/* The game saved to path (its folder made when it is missing), with thumb as its picture (SNX_STATE_THUMB_W x
   SNX_STATE_THUMB_H RGBA8, or NULL for none): 0, or -1 with the reason in why */
int snx_nds_state_save(NdsBoard* board, NdsCpu* cpu, const char* path, const uint8_t* thumb, char* why, size_t whysize);
/* The state at path put in place of the running game: 0, or -1 with the reason in why, when it was refused before
   anything changed (another game's state, another version or layout, a damaged file). Nothing after that can fail */
int snx_nds_state_load(NdsBoard* board, NdsCpu* cpu, const char* path, char* why, size_t whysize);

#endif
