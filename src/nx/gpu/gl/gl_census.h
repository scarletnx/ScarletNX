/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL call census (the gl_call_census setting, a file): which OpenGL functions a game that draws through its own GL
   driver calls, how often, and from which frame (gpu/gl/gl_census.c). Off, it takes no slot and nothing runs */
#ifndef SNX_GL_CENSUS_H
#define SNX_GL_CENSUS_H

/* the frames between two writes of the file, which is written again as the game ends */
#define SNX_GL_CENSUS_EVERY 600

/* Once the game's symbols and dispatch table are loaded, after the hooks and NVN took theirs and before any guest
   thread runs: with gl_call_census set, the slot of each function the driver exports as _gl... and of its
   eglGetProcAddress take the census's counters */
void gl_census_init(void);
/* As the game ends (snx_save_before_exit): the file written with every count so far, and the log's summary line.
   Nothing when the census is off */
void gl_census_report(void);

#endif
