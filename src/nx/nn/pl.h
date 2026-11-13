/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NX_NN_PL_H
#define SNX_NX_NN_PL_H
#include "common/cpu/aarch64/cpu.h"

/* The native nn::pl's state (g_hle.pl, which a save state holds): the shared fonts' memory, where each font is in it,
   and where the game sees it */
#define PL_FONTS 6
typedef struct {
    u32 shm;
    u32 font_off[PL_FONTS], font_size[PL_FONTS]; /* where each font starts (past its header), and its size */
    u64 base; /* the guest address of the memory, mapped read only at the first ask; 0 before */
} HlePl;

/* the fonts, read from the game folder or the stand-ins: after the file system, before the game's first thread */
void nnpl_init(void);
#endif
