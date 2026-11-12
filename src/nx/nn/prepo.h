/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NX_NN_PREPO_H
#define SNX_NX_NN_PREPO_H
#include "common/cpu/aarch64/cpu.h"

/* The native nn::prepo's state (g_hle.prepo, which a save state holds): the limit the SDK sets on how often a game
   saves a report, 900 in any 90 minutes, and the same limit of its own on the reports a game sends at once (SDK 16 on). The
   guest time (ns) of each of the last 900 saves and sends, by their count */
#define PREPO_SAVES 900
typedef struct {
    u64 saved;                /* the saves that went through */
    u64 at[PREPO_SAVES];      /* the guest time of save n at n % PREPO_SAVES */
    u64 sent;                 /* the sends that went through */
    u64 sent_at[PREPO_SAVES]; /* the guest time of send n at n % PREPO_SAVES */
} HlePrepo;

/* before hook_install */
void nnprepo_init(void);
#endif
