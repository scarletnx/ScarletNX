/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nn::pctl, native (pctl.hook): the parental controls of a console with none set, with no pctl session. The
   behavior is that of a clean-room spec of the SDK's functions, never the SDK's code.

   The SDK's start (nninitInitializeSdkModule) calls Initialize, which makes the session, and every other function
   asks the session for one command. With no restriction, each answer is a plain one: free communication and stereo
   vision are permitted, nothing is restricted, the safety level is none, and no play timer runs. The functions here
   are the ones whose answer the spec gives. Any other function opens the pctl session itself, and sm stops the game
   with the name of this layer */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include <string.h>

u32 nnpctl_initialize(CPU* c) {
    (void)c;
    return 0;
}
/* the answer of a command that succeeds with no output: true, or a Result of 0 */
int nnpctl_permitted(CPU* c, int show_error) {
    (void)c, (void)show_error;
    return 1;
}
int nnpctl_yes(CPU* c) {
    (void)c;
    return 1;
}
u32 nnpctl_success(CPU* c) {
    (void)c;
    return 0;
}
/* a flag, a count, a set of features or a level of a console with no restriction: 0 (the safety level None, no
   restricted feature, no time left on a timer that does not run) */
int nnpctl_no(CPU* c) {
    (void)c;
    return 0;
}
u32 nnpctl_none(CPU* c) {
    (void)c;
    return 0;
}
i64 nnpctl_no_time(CPU* c) {
    (void)c;
    return 0;
}
/* the play timer's display as nobody configured it: NotConfigured (7) in its first byte, the rest of its 0x18 bytes
   zero. Zeros alone read as TimesUp, and a game that believes it stops the person playing */
void nnpctl_play_timer_display_info(CPU* c, u64 info) {
    u8* p = hook_host(c, info);
    memset(p, 0, 0x18);
    p[0] = 7;
}
