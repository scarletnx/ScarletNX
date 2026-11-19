/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_PACK_H
#define SNX_PACK_H

#include "common/utilities/common.h"

/* scarletnx --pack: argv holds its arguments alone, without a program name or --pack. A folder of NCAs or a dump
   packed into an NSP or an XCI of plain NCAs (pack.c). paths gives the keys and the data folder, NULL members (or
   paths NULL) for the defaults. 0, else nonzero, said why */
int snx_pack(int argc, char** argv, const SnxPaths* paths);

#endif
