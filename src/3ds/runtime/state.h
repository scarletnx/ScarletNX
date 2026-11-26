/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A 3DS game's save states (state.c): the process (its kernel objects and handles, its threads' registers, the
   services' state), its address space's mappings, the board's RAM, the GPU's command processor and the DSP in a state
   file, the Switch's layout (nx/runtime/state_file.h), so a state's header and thumbnail read the same on every
   console. The files and archives the game has open are kept as their paths and opened again by a load; what the GPU
   keeps newer than memory lands there before a save, and a load writes the RAM through write tracking, so the
   renderer drops what it kept of the running game. The game's save data and SD card are its files, which a state never
   takes back, as a Switch game's. The runner calls both between two dispatches, on the thread the game runs on */
#ifndef SNX_CTR_STATE_H
#define SNX_CTR_STATE_H
#include "3ds/runtime/ctr_process.h"
#include <stddef.h>
#include <stdint.h>
/* what a 3DS state holds and how, in its header's abi field: a state of another version is refused, naming both */
#define SNX_CTR_STATE_VERSION 1
/* The game saved to path (its folder made when it is missing), with thumb as its picture (SNX_STATE_THUMB_W x
   SNX_STATE_THUMB_H RGBA8, or NULL for none): 0, or -1 with the reason in why. Refused, with what the game holds,
   while it holds what a state can't: sockets, local wireless (nwm::UDS), a friends request in flight, an ldr:ro command
   half done, immediate-mode vertices not drawn yet, a file open that was deleted; and once the game stopped */
int snx_ctr_state_save(SnxCtrProcess* p, const char* path, const uint8_t* thumb, char* why, size_t whysize);
/* The state at path put in place of the running game: 0, or -1 with the reason in why, when it was refused before
   anything changed (another game's state, another version or layout, a damaged file, a file or DLC content the game
   had open that can't be opened again, a friends request of the running game in flight). A failure after that (a
   mapping the space refuses) ends the game, said on stderr and in p's stop line */
int snx_ctr_state_load(SnxCtrProcess* p, const char* path, char* why, size_t whysize);
#endif
