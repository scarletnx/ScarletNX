/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_SWKBD_H
#define SNX_CTR_SWKBD_H
#include "3ds/ipc/ctr_ipc.h"
#include <stdbool.h>
#include <stdint.h>
/* the software keyboard's AppletIds: the system's (0x201) and the one an application starts (0x401) */
bool snx_ctr_swkbd_is(uint32_t applet_id);
/* SendParameter to the keyboard, but for the Request APT answers: the Message that gives what the game's own check
   made of the text, or a cancel. The command's reply is c's, a success already */
void snx_ctr_swkbd_parameter(SnxCtrIpc* c, uint32_t signal, const unsigned char* buffer, uint32_t size,
                             uint32_t object);
/* StartLibraryApplet of the keyboard: its SoftwareKeyboardConfig (size bytes) and the block its text goes in */
void snx_ctr_swkbd_start(SnxCtrIpc* c, const unsigned char* config, uint32_t size, uint32_t text);
#endif
