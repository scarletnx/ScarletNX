/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NDS_MAIN_H
#define SNX_NDS_MAIN_H
#include <stdbool.h>
/* scarletnx-nds's command line, which scarletnx runs under that name (src/multicall) */
int snx_nds_main(int argc, char** argv);
/* whether the program started as argv0 is a copy of the runtime beside an NDS game's module */
bool snx_nds_game_name(const char* argv0);
#endif
