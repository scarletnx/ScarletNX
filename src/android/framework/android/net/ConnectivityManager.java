// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.net;

/** Connectivity: the host's network, always up. */
public class ConnectivityManager {
    public static final int TYPE_MOBILE = 0, TYPE_WIFI = 1, TYPE_ETHERNET = 9;
    public static final String CONNECTIVITY_ACTION = "android.net.conn.CONNECTIVITY_CHANGE";
    public static final int RESTRICT_BACKGROUND_STATUS_DISABLED = 1;

    public static class NetworkCallback {
        public static final int FLAG_INCLUDE_LOCATION_INFO = 1;
        public NetworkCallback() {}
        public NetworkCallback(int flags) {}
        public void onPreCheck(Network network) {}
        public void onAvailable(Network network) {}
        public void onLosing(Network network, int maxMsToLive) {}
        public void onLost(Network network) {}
        public void onUnavailable() {}
        public void onCapabilitiesChanged(Network network, NetworkCapabilities networkCapabilities) {}
        public void onLinkPropertiesChanged(Network network, Object linkProperties) {}
        public void onBlockedStatusChanged(Network network, boolean blocked) {}
    }

    private final android.os.Handler handler = new android.os.Handler(android.os.Looper.getMainLooper());

    /* the host's connection while ScarletNX's Network setting is on; offline, no network at all */
    private static boolean online() { return snx.android.Network.online(); }
    public Network getActiveNetwork() { return online() ? Network.HOST : null; }
    @Deprecated public NetworkInfo getActiveNetworkInfo() { return online() ? new NetworkInfo() : null; }
    @Deprecated public NetworkInfo getNetworkInfo(Network network) { return online() ? new NetworkInfo() : null; }
    @Deprecated public NetworkInfo getNetworkInfo(int networkType) { return online() && networkType == TYPE_ETHERNET ? new NetworkInfo() : null; }
    @Deprecated public NetworkInfo[] getAllNetworkInfo() { return online() ? new NetworkInfo[] {new NetworkInfo()} : new NetworkInfo[0]; }
    public Network[] getAllNetworks() { return online() ? new Network[] {Network.HOST} : new Network[0]; }
    public NetworkCapabilities getNetworkCapabilities(Network network) { return online() && network != null ? new NetworkCapabilities() : null; }
    public boolean isActiveNetworkMetered() { return false; }
    public int getRestrictBackgroundStatus() { return RESTRICT_BACKGROUND_STATUS_DISABLED; }
    public boolean bindProcessToNetwork(Network network) { return true; }
    public Network getBoundNetworkForProcess() { return null; }
    private void available(NetworkCallback cb) {
        if (!online()) return;
        handler.post(() -> { cb.onAvailable(Network.HOST); cb.onCapabilitiesChanged(Network.HOST, new NetworkCapabilities()); });
    }
    public void registerNetworkCallback(NetworkRequest request, NetworkCallback networkCallback) { available(networkCallback); }
    public void registerNetworkCallback(NetworkRequest request, NetworkCallback networkCallback, android.os.Handler handler) { available(networkCallback); }
    public void registerDefaultNetworkCallback(NetworkCallback networkCallback) { available(networkCallback); }
    public void registerDefaultNetworkCallback(NetworkCallback networkCallback, android.os.Handler handler) { available(networkCallback); }
    public void requestNetwork(NetworkRequest request, NetworkCallback networkCallback) { available(networkCallback); }
    public void unregisterNetworkCallback(NetworkCallback networkCallback) {}
}
