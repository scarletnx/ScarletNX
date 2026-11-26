/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A Vita game's save states (state.c): the process (its threads, objects, memory blocks, fibers), guest memory,
   sceGxm, the sound ports, SceNgs and the trophies in a state file, the Switch's layout (nx/runtime/state_file.h), so a
   state's header and thumbnail read the same on every console. The files the game has open are kept as their paths and
   places and opened again by a load; the GPU's work is waited for before a save, and its renderer made again after a
   load. The game's save data (ux0:, savedata0:) is its files, which a state never takes back, as a Switch game's.
   The runner calls both between two threads' slices (no thread running), on the thread the game runs on */
#ifndef SNX_VITA_STATE_H
#define SNX_VITA_STATE_H
#include "vita/runtime/process.h"
#include <stddef.h>
#include <stdint.h>

/* what a Vita state holds and how, in its header's abi field: a state of another version is refused, naming both */
#define SNX_VITA_STATE_VERSION 1

/* The game saved to path (its folder made when it is missing), with thumb as its picture (SNX_STATE_THUMB_W x
   SNX_STATE_THUMB_H RGBA8, or NULL for none): 0, or -1 with the reason in why. Refused, with what the game holds,
   while it holds what a state can't: a thread inside a call of the runtime's into the game (a sort's comparison, a
   callback), a scene being drawn, an mp4 file or a video decoder (the movie's place and the pictures H.264 decodes the
   next from can't be put back), sockets */
int snx_vita_state_save(VitaProcess* p, const char* path, const uint8_t* thumb, char* why, size_t whysize);
/* The state at path put in place of the running game: 0, or -1 with the reason in why, when it was refused before
   anything changed (another game's state, another version or layout, a damaged file, a file the game had open that
   can't be opened again). A failure after that (the host maps no memory for it) ends the game, said on stderr */
int snx_vita_state_load(VitaProcess* p, const char* path, char* why, size_t whysize);

#endif
