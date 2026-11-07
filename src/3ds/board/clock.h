/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The console's time as the game sees it (clock.c): every deadline, sleep and tick of the 3DS runtime is on it. It is
   the host's at the console's pace; with the Switch's fps_limit=N it runs at N/60 of it, the 3DS's display refreshing
   60 times a second; with no limit (fps_limit=0) the waits the runtime would sleep are skipped, the clock moving on by
   them, so the game runs as fast as the host does. Every pace starts from where the clock stands, so a change never
   moves it back */
#ifndef SNX_CTR_CLOCK_H
#define SNX_CTR_CLOCK_H
#include <stdbool.h>
#include <stddef.h>

/* a pace: SNX_CTR_PACE_CONSOLE (fps_limit=game, the default), SNX_CTR_PACE_NONE (fps_limit=0), else 10 to 1000 frames
   a second */
enum { SNX_CTR_PACE_CONSOLE = 0, SNX_CTR_PACE_NONE = -1 };
/* the clock, in seconds */
double snx_ctr_clock_seconds(void);
/* the guest seconds that pass in a host second at the pace: 1 for the console's and for none, N/60 for N */
double snx_ctr_clock_rate(void);
/* the pace in effect, and the pace from now on */
int snx_ctr_clock_pace(void);
void snx_ctr_clock_set_pace(int pace);
/* F4's: no limit, or back from none to the pace before it (the console's when the run started with none). The pace
   in effect afterwards */
int snx_ctr_clock_toggle(void);
/* The host's wait for the clock to reach deadline (its seconds): none when it has, or with no limit, the clock moving
   to deadline at once; else the time to sleep at the pace */
double snx_ctr_clock_wait(double deadline);
/* The pace fps_limit's text says, as the Switch reads it (options_fps_set): "game" in any case the console's, a number
   below 0 or from 1 to below 2 the console's too (an older file's values), 0 to below 1 none, else that many frames a
   second held to 10 to 1000. False for anything else, with *pace as it was. line gets what is said of a value not
   taken as given, with the value in effect ("" when taken as given) */
bool snx_ctr_clock_parse(const char* text, int* pace, char* line, size_t line_size);
/* the pace as fps_limit writes it: "game", "0" or the frames a second */
void snx_ctr_clock_text(int pace, char* out, size_t size);
#endif
