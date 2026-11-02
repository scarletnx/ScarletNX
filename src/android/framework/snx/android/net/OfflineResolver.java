// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.net;

import java.net.InetAddress;
import java.net.UnknownHostException;
import java.net.spi.InetAddressResolver;
import java.net.spi.InetAddressResolverProvider;
import java.util.stream.Stream;

/**
 * The JVM's name resolver while the app is offline (snx.android.Network): localhost and addresses written as numbers
 * only, so that no name is asked of a DNS server. Online, the JVM's own. The JDK finds it through the jar's
 * META-INF/services
 */
public final class OfflineResolver extends InetAddressResolverProvider {
    @Override public InetAddressResolver get(Configuration configuration) {
        InetAddressResolver builtin = configuration.builtinResolver();
        if (snx.android.Network.online()) return builtin;
        return new InetAddressResolver() {
            @Override public Stream<InetAddress> lookupByName(String host, LookupPolicy policy) throws UnknownHostException {
                if (snx.android.Network.local(host)) return builtin.lookupByName(host, policy);
                throw new UnknownHostException(host + ": the network is off (ScarletNX's Network setting)");
            }
            @Override public String lookupByAddress(byte[] addr) throws UnknownHostException {
                throw new UnknownHostException("the network is off (ScarletNX's Network setting)");
            }
        };
    }
    @Override public String name() { return "scarletnx-offline"; }
}
