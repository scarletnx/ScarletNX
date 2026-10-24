/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Save states (state.c): the whole game written to a file, and put back in the running process, its guest threads
   going on where they were. The frontend's thread calls both */
#ifndef SNX_STATE_H
#define SNX_STATE_H
#include "nx/runtime/state_file.h"
#include <stddef.h>

/* the game saved to path: 0, or -1 with the reason in why */
int state_save(const char* path, char* why, size_t whysize);
/* the state at path put in place of the running game: 0, or -1 with the reason in why when it was refused before
   anything changed (another game's state, another runtime's layout, a damaged file). A failure after that ends the
   game, as half a state can't run */
int state_load(const char* path, char* why, size_t whysize);
/* whether the state at path can be loaded into this game: 0, or -1 with the reason (the launcher's list) */
int state_check(const SnxStateHeader* h, char* why, size_t whysize);
/* a slot's file: <g_exedir>states/<exename>.<slot>.snxstate, slot a number or "auto" */
void state_path(char* out, size_t size, const char* slot);
/* the player's: a save, load or delete of slot (0 the auto slot, -1 the one F9 picked), done at the next state_poll,
   which says what came of it in a notification */
enum { STATE_SAVE = 1, STATE_LOAD, STATE_DELETE };
void state_ask(int op, int slot);
void state_slot_next(void); /* F9: the next slot, said with what it holds */
/* at each pass of the frontend's loop: an unattended run's saves and loads (the state_save and state_load settings),
   what the player asked for, and the menu's save states pages, which hold the game while they show */
void state_poll(void);

#endif
