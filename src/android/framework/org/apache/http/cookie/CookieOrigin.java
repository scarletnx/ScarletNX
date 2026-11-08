// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.cookie;

public final class CookieOrigin {
    private final String host, path;
    private final int port;
    private final boolean secure;

    public CookieOrigin(String host, int port, String path, boolean secure) {
        if (host == null) throw new IllegalArgumentException("Host of origin may not be null");
        if (path == null) throw new IllegalArgumentException("Path of origin may not be null");
        if (port < 0) throw new IllegalArgumentException("Invalid port: " + port);
        this.host = host.toLowerCase(java.util.Locale.ENGLISH);
        this.port = port;
        this.path = path.trim().isEmpty() ? "/" : path;
        this.secure = secure;
    }
    public String getHost() { return host; }
    public String getPath() { return path; }
    public int getPort() { return port; }
    public boolean isSecure() { return secure; }
}
