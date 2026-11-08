// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http;

public class ProtocolVersion implements java.io.Serializable, Cloneable {
    protected final String protocol;
    protected final int major, minor;

    public ProtocolVersion(String protocol, int major, int minor) {
        if (protocol == null) throw new IllegalArgumentException("Protocol name may not be null");
        this.protocol = protocol;
        this.major = major;
        this.minor = minor;
    }
    public final String getProtocol() { return protocol; }
    public final int getMajor() { return major; }
    public final int getMinor() { return minor; }
    public ProtocolVersion forVersion(int major, int minor) {
        return major == this.major && minor == this.minor ? this : new ProtocolVersion(protocol, major, minor);
    }
    @Override public final int hashCode() { return protocol.hashCode() ^ major * 100000 ^ minor; }
    @Override public boolean equals(Object o) {
        return o instanceof ProtocolVersion v && protocol.equals(v.protocol) && major == v.major && minor == v.minor;
    }
    @Override public String toString() { return protocol + '/' + major + '.' + minor; }
    @Override public Object clone() throws CloneNotSupportedException { return super.clone(); }
}
