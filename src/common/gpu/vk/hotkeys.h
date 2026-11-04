/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's live keys, the same on every frontend: the spatial upscaler, the anti aliasing, HDR and frame
   interpolation. The key names are the settings' (upscaler_key, aa_key, hdr_key, frame_interp_key), and the message
   a change shows is made here, so the Switch, the 3DS, the Vita and the GBA say the same words. */
#ifndef SNX_COMMON_GPU_VK_HOTKEYS_H
#define SNX_COMMON_GPU_VK_HOTKEYS_H
#include <stddef.h>
#include <SDL3/SDL.h>

enum { SNX_HOTKEY_NONE, SNX_HOTKEY_UPSCALER, SNX_HOTKEY_AA, SNX_HOTKEY_HDR, SNX_HOTKEY_INTERP, SNX_HOTKEY_COUNT };

/* which of the four the event is, SNX_HOTKEY_NONE for another key. A repeat matches too: the frontends take it and do
   nothing, so the game never sees the key */
int snx_hotkey_match(const SDL_KeyboardEvent* event);
/* "Upscaler: AMD FSR 1", "Anti aliasing: FXAA", "HDR: On", "Frame interpolation: Auto": value is the new mode of
   action. The buffer is returned, empty for SNX_HOTKEY_NONE */
const char* snx_hotkey_text(int action, int value, char* buffer, size_t size);
/* the setting's key name of the action, for a log line */
const char* snx_hotkey_key_name(int action);
/* the frame interpolation's next mode, Off, Auto, Blend, round, written to the setting; the mode */
int snx_hotkey_next_interp(void);
#endif
