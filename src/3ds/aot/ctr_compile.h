/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_COMPILE_H
#define SNX_CTR_COMPILE_H
#include "3ds/loader/ctr_loader.h"
#include "common/utilities/cli.h"
#include <stddef.h>
/* Compiles the title's ARM11 code into the game folder directory's module (snx_ctr_game_file), replacing an earlier module once the new one is
   complete: the code of the extraction in directory, or with game the game's own, directory then the title's state
   folder. At the setting opt's level (-O), and as asked (NULL: nothing asked): --chunk's instructions to a chunk
   (512 blocks without), --jit-cache's list of the blocks a run compiled for the title's own <title ID>.hints, no blocks
   for --jit-only, each module's IR in <title ID>_build for --keep-ir, no progress for -q. 0 on success, 3 for an
   unsupported image, else 1, with error filled */
int snx_ctr_aot_compile(const char* directory, const SnxCtrSource* game, const SnxCliRecompile* asked, char* error,
                        size_t error_size);
#endif
