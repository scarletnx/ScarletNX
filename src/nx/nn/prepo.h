/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NX_NN_PREPO_H
#define SNX_NX_NN_PREPO_H
#include "common/cpu/aarch64/cpu.h"

/* The native nn::prepo's state (g_hle.prepo, which a save state holds): the limit SDK 20 sets on how often a game
   saves a report, 900 in any 90 minutes. The guest time (ns) of each of the last 900 saves, by their count */
#define PREPO_SAVES 900
typedef struct {
    u64 saved;              /* the saves that went through */
    u64 at[PREPO_SAVES];    /* the guest time of save n at n % PREPO_SAVES */
} HlePrepo;

/* before hook_install */
void nnprepo_init(void);
#endif
