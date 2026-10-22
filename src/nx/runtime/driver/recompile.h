/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_RECOMPILE_H
#define SNX_RECOMPILE_H
#include "common/utilities/common.h"
#include "nx/runtime/ini.h"
/* the ABI stamped into the games this build recompiles */
unsigned snx_game_abi(void);
/* the oldest ABI of a game this build plays without recompiling it (SNX_ABI_OLDEST) */
unsigned snx_game_abi_oldest(void);
/* what a game this build recompiled has in its <name>.abi, owned: the ABI, and " mods " and the stamp of the mods'
   ExeFS files it was built with (snx_mods_stamp, mods) unless that is "". A game whose stamp says anything else, but
   for an ABI from snx_game_abi_oldest() up, is from another version or was made with other mods, and is recompiled
   before it runs */
char* snx_game_stamp(const char* mods);
/* the runtime a game's folder gets a copy of, owned: this program, or the AppImage it runs in; NULL in a build without
   the runtime, which deploys one it builds */
char* snx_own_runtime(const SnxPaths* paths);
/* run copied to exe, the game's program (its DLLs beside it on Windows), through a temporary file it replaces exe with.
   0, or nonzero with a line saying why */
int snx_deploy_runtime(const char* run, const char* exe);
/* -r: recompiles the game the arguments name. Not snx_recompile, the recompiler library's, which the Linux runtime
   links into the same program (SNX_MULTICALL) */
int snx_recompile_game(int argc, char** argv, const SnxPaths* paths);
/* a setting of the recompile (settings.txt's recompile and llvm tables), which --KEY=VALUE sets; NULL for another key */
const IniOpt* snx_recompile_setting(const char* key);
#endif
