// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.net;

import android.content.Context;
import android.content.Intent;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import java.net.InetAddress;

/**
 * A VPN app's service (Off in docs/android/INTERFACES.md: the device makes no TUN interface). prepare gives no consent
 * screen to show, as for an app the user already allowed, and a Builder's establish fails as Android's does when the
 * interface can't be made: null. Sockets need no protecting, as no VPN routes them.
 */
public class VpnService extends android.app.Service {
    public static final String SERVICE_INTERFACE = "android.net.VpnService",
        SERVICE_META_DATA_SUPPORTS_ALWAYS_ON = "android.net.VpnService.SUPPORTS_ALWAYS_ON";

    public VpnService() {}

    public static Intent prepare(Context context) { return null; }
    public boolean protect(int socket) { return true; }
    public boolean protect(java.net.Socket socket) { return true; }
    public boolean protect(java.net.DatagramSocket socket) { return true; }
    public boolean addAddress(InetAddress address, int prefixLength) { return false; }
    public boolean removeAddress(InetAddress address, int prefixLength) { return false; }
    public boolean setUnderlyingNetworks(Network[] networks) { return true; }
    public final boolean isAlwaysOn() { return false; }
    public final boolean isLockdownEnabled() { return false; }
    @Override public IBinder onBind(Intent intent) { return null; }
    public void onRevoke() { stopSelf(); }

    public class Builder {
        private String session;
        private int mtu = 1500;
        private boolean blocking, metered = true;
        private final java.util.ArrayList<String> addresses = new java.util.ArrayList<>(), routes = new java.util.ArrayList<>(),
            dns = new java.util.ArrayList<>(), domains = new java.util.ArrayList<>(), allowed = new java.util.ArrayList<>(),
            disallowed = new java.util.ArrayList<>();

        public Builder() {}
        public Builder setSession(String session) { this.session = session; return this; }
        public Builder setConfigureIntent(android.app.PendingIntent intent) { return this; }
        public Builder setMtu(int mtu) {
            if (mtu <= 0) throw new IllegalArgumentException("Bad mtu");
            this.mtu = mtu;
            return this;
        }
        public Builder addAddress(InetAddress address, int prefixLength) { addresses.add(address.getHostAddress() + "/" + prefixLength); return this; }
        public Builder addAddress(String address, int prefixLength) { addresses.add(address + "/" + prefixLength); return this; }
        public Builder addRoute(InetAddress address, int prefixLength) { routes.add(address.getHostAddress() + "/" + prefixLength); return this; }
        public Builder addRoute(String address, int prefixLength) { routes.add(address + "/" + prefixLength); return this; }
        public Builder addDnsServer(InetAddress address) { dns.add(address.getHostAddress()); return this; }
        public Builder addDnsServer(String address) { dns.add(address); return this; }
        public Builder addSearchDomain(String domain) { domains.add(domain); return this; }
        public Builder allowFamily(int family) { return this; }
        public Builder allowBypass() { return this; }
        public Builder setBlocking(boolean blocking) { this.blocking = blocking; return this; }
        public Builder setUnderlyingNetworks(Network[] networks) { return this; }
        public Builder setMetered(boolean isMetered) { metered = isMetered; return this; }
        public Builder addAllowedApplication(String packageName) throws android.content.pm.PackageManager.NameNotFoundException {
            allowed.add(packageName);
            return this;
        }
        public Builder addDisallowedApplication(String packageName) throws android.content.pm.PackageManager.NameNotFoundException {
            disallowed.add(packageName);
            return this;
        }
        /** the interface: none can be made, as Android gives when it fails */
        public ParcelFileDescriptor establish() { return null; }
    }
}
