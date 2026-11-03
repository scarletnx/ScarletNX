// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.bluetooth;

/** The Bluetooth service. The device has no adapter. */
public final class BluetoothManager {
    private BluetoothManager() {}
    public BluetoothAdapter getAdapter() { return null; }
}
