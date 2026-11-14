/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A key as a setting holds it (Ctrl+Shift+F12): SDL's name of the key after Ctrl+, Alt+, Shift+ or GUI+ for the
   modifiers held with it, "" for no key. The Switch's runtime keys (nx/runtime/platform/hotkeys.c) and the 3DS's and
   the Vita's shortcuts read and match them the same way */
#ifndef SNX_KEY_TEXT_H
#define SNX_KEY_TEXT_H
#include <stddef.h>

/* the modifiers held with a key, either side's, in the order a key's text names them */
enum { SNX_KEY_CTRL = 1, SNX_KEY_ALT = 2, SNX_KEY_SHIFT = 4, SNX_KEY_GUI = 8 };

/* an SDL_Keymod as those bits */
unsigned snx_key_mods(unsigned sdl_mod);
/* whether key (an SDL_Keycode) is a modifier (Ctrl, Shift, Alt, GUI, AltGr...), which only goes with another */
int snx_key_modifier(unsigned key);
/* text as a key (SDL_Keycode) and the modifiers held with it: 0, "" giving no key (0); else -1 with why into why (n
   bytes): a name SDL doesn't know, a modifier where the key goes, or nothing after the modifiers */
int snx_key_parse(const char* text, unsigned* key, unsigned* mods, char* why, size_t n);
/* key and mods as a setting holds them, into out (n bytes): "" for no key */
void snx_key_name(unsigned key, unsigned mods, char* out, size_t n);
/* value read into var (size bytes), a setting key's text: 0, var then the key as snx_key_name says it; else -1, var as
   it was, and said (n bytes) says why with the key it keeps ("hotkey_upscaler=Ctrl+Foo: no key is named ...: it keeps
   Ctrl+U") */
int snx_key_set(const char* key, const char* value, char* var, size_t size, char* said, size_t n);
/* which of count keys (keys[i] with mods[i]; a key 0 is none) a key pressed with m is, -1 for none: the one set to
   that key with those modifiers, else, with Shift held, the one set to the key without Shift (Shift+F12 is F12's).
   The first in their order; another set to the same key in *also, else -1 */
int snx_key_find(const unsigned* keys, const unsigned* mods, int count, unsigned key, unsigned m, int* also);
#endif
