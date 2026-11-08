// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.net;

import java.net.InetAddress;
import java.net.UnknownHostException;
import java.net.spi.InetAddressResolver;
import java.net.spi.InetAddressResolverProvider;
import java.util.stream.Stream;

/**
 * The JVM's name resolver. The names of Android's /etc/hosts answer as on a device, online or not: localhost is
 * 127.0.0.1 alone, and ip6-localhost ::1. Offline (snx.android.Network), no other name resolves, so that none is
 * asked of a DNS server; online, the JVM's own resolver answers the others. The JDK finds it through the jar's
 * META-INF/services
 */
public final class OfflineResolver extends InetAddressResolverProvider {
    @Override public InetAddressResolver get(Configuration configuration) {
        InetAddressResolver builtin = configuration.builtinResolver();
        boolean online = snx.android.Network.online();
        return new InetAddressResolver() {
            @Override public Stream<InetAddress> lookupByName(String host, LookupPolicy policy) throws UnknownHostException {
                Stream<InetAddress> device = hosts(host, policy);
                if (device != null) return device;
                if (online) return builtin.lookupByName(host, policy);
                if (snx.android.Network.local(host)) return builtin.lookupByName(host, policy);
                throw new UnknownHostException(host + ": the network is off (ScarletNX's Network setting)");
            }
            @Override public String lookupByAddress(byte[] addr) throws UnknownHostException {
                if (online) return builtin.lookupByAddress(addr);
                throw new UnknownHostException("the network is off (ScarletNX's Network setting)");
            }
        };
    }
    @Override public String name() { return "scarletnx-offline"; }

    /**
     * A name of Android's /etc/hosts: its address, when the policy asks for its family, and none otherwise; null for
     * another name
     */
    static Stream<InetAddress> hosts(String host, InetAddressResolver.LookupPolicy policy) throws UnknownHostException {
        boolean v4 = host.equalsIgnoreCase("localhost");
        if (!v4 && !host.equalsIgnoreCase("ip6-localhost")) return null;
        int family = v4 ? InetAddressResolver.LookupPolicy.IPV4 : InetAddressResolver.LookupPolicy.IPV6;
        if ((policy.characteristics() & family) == 0)
            throw new UnknownHostException(host + ": Android's /etc/hosts gives it no address of the family asked");
        byte[] address = v4 ? new byte[] {127, 0, 0, 1} : new byte[] {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        return Stream.of(InetAddress.getByAddress(host, address));
    }
}
