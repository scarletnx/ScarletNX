// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.auth;

public class AuthScope {
    public static final String ANY_HOST = null, ANY_REALM = null, ANY_SCHEME = null;
    public static final int ANY_PORT = -1;
    public static final AuthScope ANY = new AuthScope(ANY_HOST, ANY_PORT, ANY_REALM, ANY_SCHEME);
    private final String scheme, realm, host;
    private final int port;

    public AuthScope(String host, int port, String realm, String scheme) {
        this.host = host != null ? host.toLowerCase(java.util.Locale.ENGLISH) : null;
        this.port = port < 0 ? ANY_PORT : port;
        this.realm = realm;
        this.scheme = scheme != null ? scheme.toUpperCase(java.util.Locale.ENGLISH) : null;
    }
    public AuthScope(String host, int port, String realm) { this(host, port, realm, ANY_SCHEME); }
    public AuthScope(String host, int port) { this(host, port, ANY_REALM, ANY_SCHEME); }
    public AuthScope(AuthScope authscope) { this(authscope.host, authscope.port, authscope.realm, authscope.scheme); }
    public String getHost() { return host; }
    public int getPort() { return port; }
    public String getRealm() { return realm; }
    public String getScheme() { return scheme; }
    /** how well another scope matches this one: -1 for not at all, more for a closer match */
    public int match(AuthScope that) {
        int factor = 0;
        if (java.util.Objects.equals(scheme, that.scheme)) factor += 1;
        else if (scheme != null && that.scheme != null) return -1;
        if (java.util.Objects.equals(realm, that.realm)) factor += 2;
        else if (realm != null && that.realm != null) return -1;
        if (port == that.port) factor += 4;
        else if (port != ANY_PORT && that.port != ANY_PORT) return -1;
        if (java.util.Objects.equals(host, that.host)) factor += 8;
        else if (host != null && that.host != null) return -1;
        return factor;
    }
    @Override public boolean equals(Object o) {
        return o instanceof AuthScope a && java.util.Objects.equals(host, a.host) && port == a.port
            && java.util.Objects.equals(realm, a.realm) && java.util.Objects.equals(scheme, a.scheme);
    }
    @Override public int hashCode() { return java.util.Objects.hash(host, port, realm, scheme); }
}
