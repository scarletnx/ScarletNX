/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The system's Bluetooth scan in the tests that compile the launcher (gui_sdl, gui_setup, gui_pixel_order): a fake one
   (ble_scan.h's snx_ble_fake_open) in snx_ble_system_open's place, so no test reaches BlueZ */
#ifndef SNX_TEST_BLE_FAKE_SYSTEM_H
#define SNX_TEST_BLE_FAKE_SYSTEM_H
#include "common/platform/ble_scan.h"

/* the scan snx_ble_system_open gave last (NULL before), for the test to give it adverts while it is open */
extern SnxBleScan* g_ble_fake_system;
/* snx_ble_system_open's answer: "" a fake scan, else NULL with this as why */
extern char g_ble_fake_absent[128];
#endif
