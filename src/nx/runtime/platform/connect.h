/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Controllers taking players as a console's do (gamepad_assign=1), after Eden's Modern Console (John's, Eden PR 4583):
   each controller that connects takes the lowest player free, one that leaves keeps its player for SNX_CONNECT_HOLD_NS
   and takes it back when it returns, and the game pauses once a player's controller has been gone SNX_CONNECT_PAUSE_NS,
   until it returns or + is let go on another controller, which goes on without it. The rules alone, no SDL: the
   backend (gamepad.c) tells it which controller joined or left and acts on what connect_tick says */
#ifndef SNX_CONNECT_H
#define SNX_CONNECT_H
#include "common/cpu/aarch64/cpu.h"
#include <stddef.h>

#define SNX_CONNECT_PLAYERS  8
#define SNX_CONNECT_HOLD_NS  45000000000ull /* Eden's 45 s */
#define SNX_CONNECT_PAUSE_NS 3000000000ull  /* Eden's 3 s: a pad dropped for a moment is no disconnect */
/* a controller (who: its serial, else its GUID) connected while busy's players (a bit each) hold one: the player it
   takes, its own if it left one less than SNX_CONNECT_HOLD_NS ago, else the lowest neither held nor kept; -1 for none,
   a ninth that waits */
int connect_join(const char* who, unsigned busy, u64 now_ns);
void connect_leave(int player, u64 now_ns);     /* its controller left: the player kept for it */
void connect_held(int player, const char* who); /* a player its controller already held when the mode came on */
/* kept for a controller that left, which the game still sees: SNX_CONNECT_HOLD_NS, or while a pause waits for it */
int connect_kept(int player, u64 now_ns);
enum { CONNECT_NONE, CONNECT_PAUSE, CONNECT_RESUME };
/* each poll: running, the game unpaused; pads, the controllers connected; plus, + held on one of them. What the
   backend does, CONNECT_PAUSE or CONNECT_RESUME; while a pause lasts its notice in notice (one line), and on a resume
   without a player "Going on without P2"; "" for none. freed: the players no longer kept, a bit each */
int connect_tick(u64 now_ns, int running, int pads, int plus, char* notice, size_t n, unsigned* freed);
void connect_reset(void); /* every player free, no pause */
#endif
