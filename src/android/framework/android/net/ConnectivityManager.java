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

    public Network getActiveNetwork() { return Network.HOST; }
    @Deprecated public NetworkInfo getActiveNetworkInfo() { return new NetworkInfo(); }
    @Deprecated public NetworkInfo getNetworkInfo(Network network) { return new NetworkInfo(); }
    @Deprecated public NetworkInfo[] getAllNetworkInfo() { return new NetworkInfo[] {new NetworkInfo()}; }
    public Network[] getAllNetworks() { return new Network[] {Network.HOST}; }
    public NetworkCapabilities getNetworkCapabilities(Network network) { return new NetworkCapabilities(); }
    public boolean isActiveNetworkMetered() { return false; }
    public int getRestrictBackgroundStatus() { return RESTRICT_BACKGROUND_STATUS_DISABLED; }
    public boolean bindProcessToNetwork(Network network) { return true; }
    public Network getBoundNetworkForProcess() { return null; }
    private void available(NetworkCallback cb) {
        handler.post(() -> { cb.onAvailable(Network.HOST); cb.onCapabilitiesChanged(Network.HOST, new NetworkCapabilities()); });
    }
    public void registerNetworkCallback(NetworkRequest request, NetworkCallback networkCallback) { available(networkCallback); }
    public void registerNetworkCallback(NetworkRequest request, NetworkCallback networkCallback, android.os.Handler handler) { available(networkCallback); }
    public void registerDefaultNetworkCallback(NetworkCallback networkCallback) { available(networkCallback); }
    public void registerDefaultNetworkCallback(NetworkCallback networkCallback, android.os.Handler handler) { available(networkCallback); }
    public void requestNetwork(NetworkRequest request, NetworkCallback networkCallback) { available(networkCallback); }
    public void unregisterNetworkCallback(NetworkCallback networkCallback) {}
}
