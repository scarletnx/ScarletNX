/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_MII_SELECTOR_H
#define SNX_CTR_MII_SELECTOR_H
#include "3ds/ipc/ctr_ipc.h"
#include <stdbool.h>
#include <stdint.h>
/* the Mii selector's AppletIds: the system's (0x202) and the one an application starts (0x402) */
bool snx_ctr_mii_selector_is(uint32_t applet_id);
/* StartLibraryApplet of the Mii selector: its MiiConfig (size bytes). It answers at once with ScarletNX's own Mii and
   closes, and the application wakes with the MiiResult. The command's reply is c's, a success already */
void snx_ctr_mii_selector_start(SnxCtrIpc* c, const unsigned char* config, uint32_t size);
#endif
