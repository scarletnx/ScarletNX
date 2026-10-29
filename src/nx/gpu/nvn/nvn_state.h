/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct's state, as the HLE's is g_hle (hle/hle.h): every variable of static duration the files of gpu/nvn/ keep
   is a member of g_nvn, one struct per file (g_nvn.cmd is nvn_cmd.c's), whose type is declared in the header beside
   that file. (nvn.h is the vendored NVN header, so the global has this header of its own.)
   g_nvn has no initializer, so it is all zero in .bss: nvn_init sets what starts out otherwise (the locks), first
   thing in the program, before anything reads or writes g_nvn. Only .c files include this header */
#ifndef SNX_NVN_STATE_H
#define SNX_NVN_STATE_H
#include "nx/gpu/nvn/nvn_cmd.h"
#include "nx/gpu/nvn/nvn_copy.h"
#include "nx/gpu/nvn/nvn_direct.h"
#include "nx/gpu/nvn/nvn_draw.h"
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_queue.h"
#include "nx/gpu/nvn/nvn_replay.h"
#include "nx/gpu/nvn/nvn_verify.h"

typedef struct {
    NvnDirectState direct;
    NvnObjectState object;
    NvnCmdState cmd;
    NvnQueueState queue;
    NvnReplayState replay;
    NvnDrawState draw;
    NvnCopyState copy;
    NvnVerifyState verify;
} Nvn;

extern Nvn g_nvn;
void nvn_init(void);
#endif
