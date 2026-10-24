/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game paused as a whole (pause.c): every guest thread, the GPU, host1x, the audio clock and guest time stop
   together. The frontend's thread calls all of it */
#ifndef SNX_PAUSE_H
#define SNX_PAUSE_H
#include "common/cpu/aarch64/cpu.h"

/* Pauses nest: each game_pause that returned 0 takes a game_resume. -1 when something didn't stop in time, logged,
   and the game goes on unpaused */
int game_pause(void);
void game_resume(void);
int game_paused(void); /* any thread */
/* the player's pause (the menu's Pause, the Pause key), on or off */
void game_pause_toggle(void);
/* an unattended run's pauses (the pause setting), at each pass of the frontend's loop */
void pause_unattended(void);
/* a hash of every readable guest page, without landing what the GPU owns (whose owners are hashed instead), and the
   bytes read: what an unattended pause compares at its start and end */
u64 pause_memory_hash(u64* bytes);

#endif
