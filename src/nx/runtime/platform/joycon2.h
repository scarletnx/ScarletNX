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
/* For the controller applet's dialog, whose scan hears the controllers (ctrl_scan.h): whether the host runs, whether
   it knows a controller, and a controller heard to connect to now, of side (JC2_SIDE_*: a Joy-Con 2's or a Pro
   Controller 2), a new one waiting to sync only once the user confirmed it (the sync window opens for it) */
int joycon2_running(void);
int joycon2_known(const u8 address[6]);
void joycon2_connect(const u8 address[6], int random, int side, int syncing);
/* The dialog's Disconnect: the controller (both halves of a pair) behind an SDL gamepad id has only its link closed,
   no command sent, and is held off from connecting again for a while: the dialog's scan and the tries in turn leave
   it, joycon2_connect takes it back at once, and after the hold a button press connects it as before. How many
   controllers it closes. And whether a known controller heard is held off (its advert logged, wake: its flag) */
int joycon2_disconnect(u32 gamepad_id);
int joycon2_held(const u8 address[6], int wake);
/* the dialog's scan listens: the known controllers are not tried in turn meanwhile, it hears them. And whether a
   connect it asked for still goes on */
void joycon2_listening(int on);
int joycon2_connecting(void);
int joycon2_has(const u8 address[6]); /* a controller at address is connected */
#endif
