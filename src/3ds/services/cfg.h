/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_CFG_H
#define SNX_CTR_CFG_H
#include "3ds/ipc/ctr_ipc.h"
void snx_ctr_cfg_dispatch(SnxCtrIpc* c);
#define SNX_CTR_CFG_USERNAME 10u /* the console's user name, in UTF-16 units */
/* the console's user name, as the config block 0x000A0000 holds it: the user playing's nickname, else "Player", cut
   to its ten units; returns how many */
uint32_t snx_ctr_cfg_username(uint16_t out[SNX_CTR_CFG_USERNAME]);
#endif
