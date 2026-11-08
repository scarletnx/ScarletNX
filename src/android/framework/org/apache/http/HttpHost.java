// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http;

public final class HttpHost implements Cloneable {
    public static final String DEFAULT_SCHEME_NAME = "http";
    private final String hostname, schemeName;
    private final int port;

    public HttpHost(String hostname, int port, String scheme) {
        if (hostname == null) throw new IllegalArgumentException("Host name may not be null");
        this.hostname = hostname;
        this.port = port;
        this.schemeName = scheme != null ? scheme.toLowerCase(java.util.Locale.ENGLISH) : DEFAULT_SCHEME_NAME;
    }
    public HttpHost(String hostname, int port) { this(hostname, port, null); }
    public HttpHost(String hostname) { this(hostname, -1, null); }
    public HttpHost(HttpHost httphost) { this(httphost.hostname, httphost.port, httphost.schemeName); }
    public String getHostName() { return hostname; }
    public int getPort() { return port; }
    public String getSchemeName() { return schemeName; }
    public String toURI() { return schemeName + "://" + toHostString(); }
    public String toHostString() { return port != -1 ? hostname + ":" + port : hostname; }
    @Override public String toString() { return toURI(); }
    @Override public boolean equals(Object o) {
        return o instanceof HttpHost h && hostname.equalsIgnoreCase(h.hostname) && port == h.port && schemeName.equals(h.schemeName);
    }
    @Override public int hashCode() { return hostname.toLowerCase(java.util.Locale.ENGLISH).hashCode() ^ port ^ schemeName.hashCode(); }
    @Override public Object clone() throws CloneNotSupportedException { return super.clone(); }
}
