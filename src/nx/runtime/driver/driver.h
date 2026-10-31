/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_DRIVER_H
#define SNX_DRIVER_H
/* -1 leaves the arguments to the runtime; other results are exit codes. */
int snx_driver(int argc, char** argv);
/* 1 when key is a setting of the game's .ini (runtime/options.c); weak, as a build of the driver alone has no runtime
   and so no setting */
int snx_setting_known(const char* key) __attribute__((weak));
/* scarletnx --probe, Recommended Settings' test of this computer (runtime/main.c): its exit code; weak, as the driver
   alone has no GPU to test */
int snx_probe(void) __attribute__((weak));
#endif
