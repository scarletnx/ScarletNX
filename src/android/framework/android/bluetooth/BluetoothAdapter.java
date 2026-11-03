// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.bluetooth;

import android.content.Context;

/** The Bluetooth radio (Off in docs/android/INTERFACES.md). The device has none, so getDefaultAdapter answers null. */
public final class BluetoothAdapter {
    public static final int STATE_OFF = 10, STATE_TURNING_ON = 11, STATE_ON = 12, STATE_TURNING_OFF = 13;
    public static final String ACTION_STATE_CHANGED = "android.bluetooth.adapter.action.STATE_CHANGED";
    public static final String EXTRA_STATE = "android.bluetooth.adapter.extra.STATE";

    private BluetoothAdapter() {}
    public static BluetoothAdapter getDefaultAdapter() { return null; }
    public boolean isEnabled() { return false; }
    public int getState() { return STATE_OFF; }
    public boolean getProfileProxy(Context context, BluetoothProfile.ServiceListener listener, int profile) { return false; }
    public void closeProfileProxy(int profile, BluetoothProfile proxy) {}
}
