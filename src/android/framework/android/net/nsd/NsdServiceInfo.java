// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.net.nsd;

import android.os.Parcel;
import android.os.Parcelable;
import java.net.InetAddress;
import java.util.Collections;
import java.util.HashMap;
import java.util.Map;

/** A network service, as DNS-SD names it: its name, type, host and port, and its TXT attributes. */
public final class NsdServiceInfo implements Parcelable {
    private String serviceName, serviceType;
    private InetAddress host;
    private int port;
    private final HashMap<String, byte[]> attributes = new HashMap<>();

    public NsdServiceInfo() {}

    public String getServiceName() { return serviceName; }
    public void setServiceName(String s) { serviceName = s; }
    public String getServiceType() { return serviceType; }
    public void setServiceType(String s) { serviceType = s; }
    @Deprecated public InetAddress getHost() { return host; }
    @Deprecated public void setHost(InetAddress s) { host = s; }
    public int getPort() { return port; }
    public void setPort(int p) { port = p; }

    public void setAttribute(String key, String value) {
        if (key == null || key.isEmpty() || key.contains("=")) throw new IllegalArgumentException("Invalid attribute key " + key);
        attributes.put(key, value != null ? value.getBytes(java.nio.charset.StandardCharsets.UTF_8) : null);
    }
    public void removeAttribute(String key) { attributes.remove(key); }
    public Map<String, byte[]> getAttributes() { return Collections.unmodifiableMap(attributes); }

    @Override public String toString() { return "name: " + serviceName + ", type: " + serviceType + ", host: " + host + ", port: " + port; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<NsdServiceInfo> CREATOR = new Parcelable.Creator<>() {
        @Override public NsdServiceInfo createFromParcel(Parcel in) { return (NsdServiceInfo) in.readValue(null); }
        @Override public NsdServiceInfo[] newArray(int size) { return new NsdServiceInfo[size]; }
    };
}
