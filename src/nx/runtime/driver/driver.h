/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_DRIVER_H
#define SNX_DRIVER_H
/* -1 leaves the arguments to the runtime; other results are exit codes. */
int snx_driver(int argc, char** argv);
/* 1 when key is a setting of the game's .ini (runtime/options.c); weak, as a build of the driver alone has no runtime
   and so no setting */
[[gnu::weak]] int snx_setting_known(const char* key);
#endif
