// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.net.wifi.p2p;

/** Wi-Fi Direct on a device without Wi-Fi. */
public class WifiP2pManager {
    public static final String WIFI_P2P_STATE_CHANGED_ACTION = "android.net.wifi.p2p.STATE_CHANGED",
        EXTRA_WIFI_STATE = "wifi_p2p_state";
    public static final int WIFI_P2P_STATE_DISABLED = 1, WIFI_P2P_STATE_ENABLED = 2;
    public static final int ERROR = 0, P2P_UNSUPPORTED = 1, BUSY = 2;

    public WifiP2pManager() {}
}
