/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GUI_RECENT_H
#define SNX_GUI_RECENT_H
#include <stddef.h>

/* The games the GUI ran last, newest first, as the menu offers them above its items */
#define GUI_RECENT_MAX 5
typedef struct {
    char path[1024]; /* absolute */
    char name[128];  /* what the run called it, else the file's own name */
    int driver;      /* GUI_SWITCH, GUI_LINUX, GUI_3DS, GUI_VITA or GUI_GBA: the item that loaded it */
    int prepared;    /* a program from games/, run as it is */
} GuiRecentGame;
typedef struct {
    GuiRecentGame game[GUI_RECENT_MAX];
    int count;
} GuiRecent;

/* Puts a game first, named after its file, moving it up when it is there already and dropping the oldest past the
   maximum; a path or name that would not fit one line of the file is left out. 0 when added */
int gui_recent_add(GuiRecent* recent, const char* path, int driver, int prepared);
void gui_recent_remove(GuiRecent* recent, int index);
/* The name the run gave the game at path; 1 when it changed */
int gui_recent_rename(GuiRecent* recent, const char* path, const char* name);
/* The dump at index made since into the game whose program is at path: the entry runs that program from then on, its
   place and name kept, and an entry of the program already there is dropped, the newer of the two places kept. A path
   that would not fit one line of the file is left out, as gui_recent_add does. 1 when the list changed */
int gui_recent_made(GuiRecent* recent, int index, const char* path);
/* The file: a line per game, "<switch|linux|3ds|vita> <file|games> <path> <name>" separated by tabs. Loading keeps the games
   whose file is still there and skips a line it cannot read; a missing list is an empty one */
void gui_recent_load(GuiRecent* recent, const char* file);
int gui_recent_save(const GuiRecent* recent, const char* file);
#endif
