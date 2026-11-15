/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "ble_fake_system.h"
#include <stdio.h>

SnxBleScan* g_ble_fake_system;
char g_ble_fake_absent[128];
SnxBleScan* snx_ble_system_open(char* why, size_t size) {
    if (g_ble_fake_absent[0]) {
        snprintf(why, size, "%s", g_ble_fake_absent);
        return NULL;
    }
    g_ble_fake_system = snx_ble_fake_open("");
    if (!g_ble_fake_system) snprintf(why, size, "%s", "out of memory");
    return g_ble_fake_system;
}
