// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.net;

import android.os.Parcel;
import android.os.Parcelable;
import java.io.IOException;
import java.net.InetAddress;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

/**
 * The properties of the host's network link. The DNS servers are those of the host's resolv.conf, where the host
 * has one; private DNS is off.
 */
public final class LinkProperties implements Parcelable {
    private final List<InetAddress> dnsServers = new ArrayList<>();

    public LinkProperties() {
        try {
            for (String line : Files.readAllLines(Path.of("/etc/resolv.conf"))) {
                String[] f = line.trim().split("\\s+");
                if (f.length >= 2 && f[0].equals("nameserver") && f[1].matches("[0-9a-fA-F.:]+")) dnsServers.add(InetAddress.getByName(f[1]));
            }
        } catch (IOException | RuntimeException e) {
        }
    }

    public String getInterfaceName() { return "eth0"; }
    public List<InetAddress> getDnsServers() { return new ArrayList<>(dnsServers); }
    public boolean isPrivateDnsActive() { return false; }
    public String getPrivateDnsServerName() { return null; }
    public String getDomains() { return null; }
    public int getMtu() { return 0; }
    public InetAddress getDhcpServerAddress() { return null; }
    public boolean isWakeOnLanSupported() { return false; }

    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<LinkProperties> CREATOR = new Parcelable.Creator<>() {
        @Override public LinkProperties createFromParcel(Parcel in) { return (LinkProperties) in.readValue(null); }
        @Override public LinkProperties[] newArray(int size) { return new LinkProperties[size]; }
    };
}
