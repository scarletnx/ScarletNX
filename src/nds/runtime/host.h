/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The run's side of the host: the window with both screens (the GPU's presenter draws into it, gpu/vk), the keys, the
   on-screen controls and the pen, the sound's output. */
#ifndef SNX_NDS_HOST_H
#define SNX_NDS_HOST_H
#include "nds/board/nds.h"

/* what the on-screen controls say, on stderr */
void nds_host_say(const char* line);
typedef struct NdsHost NdsHost;
typedef struct {
    uint16_t keys;             /* held: KEYINPUT's ten bits, 0x400 X, 0x800 Y */
    uint16_t touch_x, touch_y; /* the pen on the lower screen */
    uint8_t touching;
} NdsInput;
/* headless: no window and no sound device, frames only where --shot asks. ini: the game's, which keeps the on-screen
   controls' layout (NULL: none kept), the DS's (vpad_consoles.h), whose settings are read by then (vpad_settings). wav,
   when not NULL, is a file the run's sound is written to as well. NULL with the reason in error */
NdsHost* nds_host_open(NdsBoard* board, const char* title, const char* ini, const char* wav, int headless,
                       int fullscreen, char* error, size_t error_size);
/* the window's events, the keys and the pen after them, none while the on-screen controls' layout is edited: false once
   it is closed */
bool nds_host_poll(NdsHost* host, NdsInput* input);
/* a frame of the console's is done: its sound goes out, and the run waits for the frame's time */
void nds_host_frame(NdsHost* host);
/* the last frame, both screens, as a PPM at path: 0, or -1 */
int nds_host_shot(NdsHost* host, const char* path);
void nds_host_close(NdsHost* host);
#endif
