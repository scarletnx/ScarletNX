// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.net;

/** A network: the host's. */
public class Network implements android.os.Parcelable {
    public static final Network HOST = new Network(100);
    private final int netId;
    public Network(int netId) { this.netId = netId; }
    public java.net.InetAddress[] getAllByName(String host) throws java.net.UnknownHostException { return java.net.InetAddress.getAllByName(host); }
    public java.net.InetAddress getByName(String host) throws java.net.UnknownHostException { return java.net.InetAddress.getByName(host); }
    public java.net.URLConnection openConnection(java.net.URL url) throws java.io.IOException { return url.openConnection(); }
    public long getNetworkHandle() { return netId; }
    @Override public boolean equals(Object o) { return o instanceof Network n && n.netId == netId; }
    @Override public int hashCode() { return netId; }
    @Override public String toString() { return Integer.toString(netId); }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeInt(netId); }
}
