/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A GBA game's save states (state.c): the board, the CPU and the console's memory in a state file, the Switch's
   layout (nx/runtime/state_file.h), so a state's header and thumbnail read the same on every console. The runner calls
   both between two frames, on the thread the machine runs on. The cartridge's save memory is the game's save file
   (<name>.sav), which a state never takes back, as a Switch game's save files */
#ifndef SNX_AGB_STATE_H
#define SNX_AGB_STATE_H
#include "agb/board/board.h"
#include "agb/cpu/agb_cpu.h"
#include <stddef.h>
#include <stdint.h>

/* what a GBA state holds and how, in its header's abi field: a state of another version is refused, naming both */
#define SNX_AGB_STATE_VERSION 1

/* The game saved to path (its folder made when it is missing), with thumb as its picture (SNX_STATE_THUMB_W x
   SNX_STATE_THUMB_H RGBA8, or NULL for none): 0, or -1 with the reason in why */
int snx_agb_state_save(AgbBoard* board, AgbCpu* cpu, const char* path, const uint8_t* thumb, char* why, size_t whysize);
/* The state at path put in place of the running game: 0, or -1 with the reason in why, when it was refused before
   anything changed (another game's state, another version or layout, a damaged file). Nothing after that can fail */
int snx_agb_state_load(AgbBoard* board, AgbCpu* cpu, const char* path, char* why, size_t whysize);

#endif
