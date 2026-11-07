// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android;

import java.io.IOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Proxy;
import java.net.ProxySelector;
import java.net.SocketAddress;
import java.net.URI;
import java.util.List;

/**
 * Whether the app has the network: ScarletNX's Network setting (settings.txt's network: 1 the computer's connection, 0
 * offline with loopback only), which the runtime gives as snx.android.network. Offline, no name resolves but
 * localhost's (OfflineResolver), a connection to another host fails, and ConnectivityManager reports no network.
 */
public final class Network {
    private static final boolean online = "1".equals(System.getProperty("snx.android.network", "0"));

    private Network() {}

    public static boolean online() { return online; }

    /** offline: every connection that a proxy selector decides refused, as with no route to the host */
    static void install() {
        if (online) return;
        ProxySelector.setDefault(new ProxySelector() {
            @Override public List<Proxy> select(URI uri) {
                String host = uri.getHost();
                if (host != null && local(host)) return List.of(Proxy.NO_PROXY);
                /* a SOCKS proxy at the discard port of loopback, where nothing listens: the connection is refused */
                return List.of(new Proxy(Proxy.Type.SOCKS, new InetSocketAddress(InetAddress.getLoopbackAddress(), 9)));
            }
            @Override public void connectFailed(URI uri, SocketAddress sa, IOException ioe) {}
        });
    }

    /** a name or an address of this machine's loopback */
    public static boolean local(String host) {
        String h = host.startsWith("[") && host.endsWith("]") ? host.substring(1, host.length() - 1) : host;
        return h.equalsIgnoreCase("localhost") || h.startsWith("127.") || h.equals("::1") || h.equals("0:0:0:0:0:0:0:1");
    }
}
