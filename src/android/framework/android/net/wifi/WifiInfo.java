// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.net.wifi;

import android.net.TransportInfo;
import android.os.Parcel;
import android.os.Parcelable;

/** The Wi-Fi connection: none, as Android reports it while Wi-Fi is not connected. */
public class WifiInfo implements TransportInfo, Parcelable {
    public static final String LINK_SPEED_UNITS = "Mbps", FREQUENCY_UNITS = "MHz";
    public static final int LINK_SPEED_UNKNOWN = -1;
    static final int INVALID_RSSI = -127;

    WifiInfo() {}

    public String getSSID() { return "<unknown ssid>"; }
    public String getBSSID() { return null; }
    public int getRssi() { return INVALID_RSSI; }
    public int getLinkSpeed() { return LINK_SPEED_UNKNOWN; }
    public int getFrequency() { return -1; }
    public String getMacAddress() { return "02:00:00:00:00:00"; }
    @Deprecated public int getIpAddress() { return 0; }
    public int getNetworkId() { return -1; }
    public boolean getHiddenSSID() { return false; }

    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) {}
    public static final Parcelable.Creator<WifiInfo> CREATOR = new Parcelable.Creator<>() {
        @Override public WifiInfo createFromParcel(Parcel in) { return new WifiInfo(); }
        @Override public WifiInfo[] newArray(int size) { return new WifiInfo[size]; }
    };
}
