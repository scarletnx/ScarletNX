// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.net;

import java.net.InetAddress;

/** Numeric addresses: IPv4 dotted quads and IPv6 literals, never a name to look up */
public final class InetAddresses {
    private InetAddresses() {}

    public static boolean isNumericAddress(String address) {
        if (address == null || address.isEmpty()) return false;
        if (address.indexOf(':') >= 0) {
            String a = address.startsWith("[") && address.endsWith("]") ? address.substring(1, address.length() - 1) : address;
            if (!a.matches("[0-9A-Fa-f:.%a-zA-Z_]+")) return false;
            try {
                return InetAddress.getByName(a) instanceof java.net.Inet6Address;
            } catch (java.net.UnknownHostException e) {
                return false;
            }
        }
        String[] parts = address.split("\\.", -1);
        if (parts.length != 4) return false;
        for (String p : parts) {
            if (p.isEmpty() || p.length() > 3 || !p.chars().allMatch(Character::isDigit) || Integer.parseInt(p) > 255) return false;
        }
        return true;
    }
    public static InetAddress parseNumericAddress(String address) {
        if (!isNumericAddress(address)) throw new IllegalArgumentException("Not a numeric address: " + address);
        try {
            String a = address.startsWith("[") ? address.substring(1, address.length() - 1) : address;
            return InetAddress.getByName(a);
        } catch (java.net.UnknownHostException e) {
            throw new IllegalArgumentException("Not a numeric address: " + address, e);
        }
    }
}
