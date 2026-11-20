/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_DRIVER_H
#define SNX_DRIVER_H
/* -1 leaves the arguments to the runtime; other results are exit codes. */
int snx_driver(int argc, char** argv);
/* 1 when key is a setting of the game's .ini: runtime/options.c's in a program with the runtime, and driver.c's own in
   one of the driver alone, which has no setting */
int snx_setting_known(const char* key);
#endif
