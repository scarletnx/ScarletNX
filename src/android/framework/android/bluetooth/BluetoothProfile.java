// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.bluetooth;

import java.util.List;

/** A Bluetooth profile's proxy. */
public interface BluetoothProfile {
    int HEADSET = 1, A2DP = 2, HEALTH = 3, HID_DEVICE = 19, HEARING_AID = 21, LE_AUDIO = 22;
    int STATE_DISCONNECTED = 0, STATE_CONNECTING = 1, STATE_CONNECTED = 2, STATE_DISCONNECTING = 3;
    String EXTRA_STATE = "android.bluetooth.profile.extra.STATE";
    String EXTRA_PREVIOUS_STATE = "android.bluetooth.profile.extra.PREVIOUS_STATE";

    List<BluetoothDevice> getConnectedDevices();
    List<BluetoothDevice> getDevicesMatchingConnectionStates(int[] states);
    int getConnectionState(BluetoothDevice device);

    interface ServiceListener {
        void onServiceConnected(int profile, BluetoothProfile proxy);
        void onServiceDisconnected(int profile);
    }
}
