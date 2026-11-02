// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.telephony;

/** A listener of the phone's state: the device has no phone, so no call comes */
@Deprecated
public class PhoneStateListener {
    public static final int LISTEN_NONE = 0, LISTEN_SERVICE_STATE = 1, LISTEN_SIGNAL_STRENGTH = 2,
        LISTEN_CALL_STATE = 0x20, LISTEN_DATA_CONNECTION_STATE = 0x40, LISTEN_DATA_ACTIVITY = 0x80,
        LISTEN_SIGNAL_STRENGTHS = 0x100, LISTEN_CELL_INFO = 0x400;

    public PhoneStateListener() {}
    public PhoneStateListener(java.util.concurrent.Executor executor) {}
    public void onCallStateChanged(int state, String phoneNumber) {}
    public void onSignalStrengthsChanged(SignalStrength signalStrength) {}
    public void onDataConnectionStateChanged(int state) {}
    public void onDataActivity(int direction) {}
    public void onServiceStateChanged(Object serviceState) {}
}
