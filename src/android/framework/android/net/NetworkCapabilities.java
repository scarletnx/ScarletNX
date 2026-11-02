// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.net;

/** The host network's capabilities: internet, validated, unmetered. */
public final class NetworkCapabilities implements android.os.Parcelable {
    public static final int NET_CAPABILITY_MMS = 0, NET_CAPABILITY_NOT_METERED = 11, NET_CAPABILITY_INTERNET = 12,
        NET_CAPABILITY_NOT_RESTRICTED = 13, NET_CAPABILITY_TRUSTED = 14, NET_CAPABILITY_NOT_VPN = 15, NET_CAPABILITY_VALIDATED = 16,
        NET_CAPABILITY_CAPTIVE_PORTAL = 17, NET_CAPABILITY_NOT_ROAMING = 18, NET_CAPABILITY_FOREGROUND = 19,
        NET_CAPABILITY_NOT_CONGESTED = 20, NET_CAPABILITY_NOT_SUSPENDED = 21, NET_CAPABILITY_TEMPORARILY_NOT_METERED = 25;
    public static final int TRANSPORT_CELLULAR = 0, TRANSPORT_WIFI = 1, TRANSPORT_BLUETOOTH = 2, TRANSPORT_ETHERNET = 3, TRANSPORT_VPN = 4;
    public NetworkCapabilities() {}
    public NetworkCapabilities(NetworkCapabilities nc) {}
    public boolean hasCapability(int capability) {
        return capability == NET_CAPABILITY_INTERNET || capability == NET_CAPABILITY_VALIDATED || capability == NET_CAPABILITY_NOT_METERED
            || capability == NET_CAPABILITY_NOT_RESTRICTED || capability == NET_CAPABILITY_TRUSTED || capability == NET_CAPABILITY_NOT_VPN
            || capability == NET_CAPABILITY_NOT_ROAMING || capability == NET_CAPABILITY_FOREGROUND || capability == NET_CAPABILITY_NOT_CONGESTED
            || capability == NET_CAPABILITY_NOT_SUSPENDED;
    }
    public boolean hasTransport(int transportType) { return transportType == TRANSPORT_ETHERNET; }
    public int[] getCapabilities() { return new int[] {NET_CAPABILITY_INTERNET, NET_CAPABILITY_VALIDATED, NET_CAPABILITY_NOT_METERED}; }
    public int getLinkDownstreamBandwidthKbps() { return 1000000; }
    public int getLinkUpstreamBandwidthKbps() { return 1000000; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) {}
}
