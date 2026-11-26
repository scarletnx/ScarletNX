/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A key as a setting holds it (settings.txt's Shortcuts): SDL's name of the key after Ctrl+, Alt+, Shift+ or GUI+ for
   the modifiers held with it (Ctrl+Shift+F12), "" for none. The runtime's keys (common/utilities/hotkeys.c) and the
   launcher's Shortcuts rows, which take a key pressed, read and write them alike */
#ifndef SNX_KEY_TEXT_H
#define SNX_KEY_TEXT_H
#include <stddef.h>

/* the modifiers held with a key, either side's, in the order a key's text names them */
enum { SNX_KEY_CTRL = 1, SNX_KEY_ALT = 2, SNX_KEY_SHIFT = 4, SNX_KEY_GUI = 8 };
/* whether key (an SDL_Keycode) is a modifier (Ctrl, Shift, Alt, GUI, AltGr...), which only goes with another */
int snx_key_modifier(unsigned key);
/* SDL's modifiers held (an SDL_Keymod) as SNX_KEY_ bits */
unsigned snx_key_mods(unsigned sdl_mods);
/* text as a key (SDL_Keycode) and the modifiers held with it: 0, "" giving no key (0); else -1 with why into why (n
   bytes): a name SDL doesn't know, a modifier where the key goes, or nothing after the modifiers */
int snx_key_parse(const char* text, unsigned* key, unsigned* mods, char* why, size_t n);
/* key and mods as a setting holds them, into out (n bytes): "" for no key */
void snx_key_name(unsigned key, unsigned mods, char* out, size_t n);
#endif
