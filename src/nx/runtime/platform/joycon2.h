/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Joy-Con 2 over Bluetooth LE, the runtime's own host (setting joycon2, off by default; Linux). A thread finds the
   controllers, connects to each without pairing it (so it stays paired with its console), sets it up and reads it
   (joycon2_session.h, on joycon2_att.h's link). The main thread makes each controller that is ready an SDL virtual
   gamepad, a left and a right one together when gamepad_joycon_pair is on (joycon2_sdl.h), and gives it its player's
   lights and the game's vibration.

   A controller is taken when it waits to sync (its sync button: its advert names no console) while the sync window
   is open (joycon2_sync), or any time when it is one the runtime knows: those it took before, kept in joycon2.txt of
   the user's config folder (address, L or R, serial). NOT FINISHED: no scanner finds controllers yet (joycon2.c's
   scan_start gives the design), so the thread only tries the addresses joycon2.txt lists. It ran against a left and
   a right Joy-Con 2 on Linux */
#ifndef SNX_JOYCON2_H
#define SNX_JOYCON2_H
#include "common/cpu/aarch64/cpu.h"

void joycon2_start(void); /* at startup, after the gamepads' scan: the host when joycon2 is on */
void joycon2_stop(void);
/* each poll on the main thread, before its events: the controllers that became ready are gamepads, those that left
   are not, and the gamepads get their controllers' state */
void joycon2_poll(void);
/* for seconds from now, a controller that waits to sync is taken and remembered */
void joycon2_sync(u64 seconds);
#endif
