/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The system's scan where there is none yet (ble_scan.h). Windows' is designed, not written: a C++/WinRT file of its
   own, BluetoothLEAdvertisementWatcher with ScanningMode Active and a manufacturer data filter of the company, its
   Received events queued under a lock for poll (BluetoothAddress and BluetoothAddressType, RawSignalStrengthInDBm,
   the manufacturer data's bytes, LocalName), Stop on stop. It needs no pairing either: the runtime would connect with
   BluetoothLEDevice.FromBluetoothAddressAsync and a GattSession (joycon2.c's design comment) */
#include "common/platform/ble_scan.h"
#include <stdio.h>

SnxBleScan* snx_ble_system_open(char* why, size_t size) {
#ifdef _WIN32
    snprintf(why, size, "%s", "searching for Bluetooth controllers is not written for Windows yet");
#else
    snprintf(why, size, "%s", "searching for Bluetooth controllers is not written for this system");
#endif
    return NULL;
}
