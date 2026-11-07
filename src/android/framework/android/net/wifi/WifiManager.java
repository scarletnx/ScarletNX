// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.net.wifi;

/** Wi-Fi on a device without Wi-Fi: the host's network is wired. */
public class WifiManager {
    public static final int WIFI_STATE_DISABLING = 0, WIFI_STATE_DISABLED = 1, WIFI_STATE_ENABLING = 2,
        WIFI_STATE_ENABLED = 3, WIFI_STATE_UNKNOWN = 4;
    public static final String WIFI_STATE_CHANGED_ACTION = "android.net.wifi.WIFI_STATE_CHANGED",
        NETWORK_STATE_CHANGED_ACTION = "android.net.wifi.STATE_CHANGE", RSSI_CHANGED_ACTION = "android.net.wifi.RSSI_CHANGED",
        SCAN_RESULTS_AVAILABLE_ACTION = "android.net.wifi.SCAN_RESULTS", EXTRA_WIFI_STATE = "wifi_state";

    public WifiManager() {}

    public boolean isWifiEnabled() { return false; }
    public int getWifiState() { return WIFI_STATE_DISABLED; }
    @Deprecated public boolean setWifiEnabled(boolean enabled) { return false; }
    @Deprecated public WifiInfo getConnectionInfo() { return new WifiInfo(); }
    @Deprecated public boolean startScan() { return false; }
    public boolean is5GHzBandSupported() { return false; }
    public static int calculateSignalLevel(int rssi, int numLevels) {
        if (rssi <= -100) return 0;
        if (rssi >= -55) return numLevels - 1;
        return (int) ((float) (rssi + 100) * (numLevels - 1) / 45);
    }
}
