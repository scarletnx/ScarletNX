// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.conn.scheme;

public final class Scheme {
    private final String name;
    private final SocketFactory socketFactory;
    private final int defaultPort;
    private final boolean layered;

    public Scheme(String name, SocketFactory factory, int port) {
        if (name == null) throw new IllegalArgumentException("Scheme name may not be null");
        if (factory == null) throw new IllegalArgumentException("Socket factory may not be null");
        if (port <= 0 || port > 0xffff) throw new IllegalArgumentException("Port is invalid: " + port);
        this.name = name.toLowerCase(java.util.Locale.ENGLISH);
        socketFactory = factory;
        defaultPort = port;
        layered = factory instanceof LayeredSocketFactory;
    }
    public int getDefaultPort() { return defaultPort; }
    public SocketFactory getSocketFactory() { return socketFactory; }
    public String getName() { return name; }
    public boolean isLayered() { return layered; }
    public int resolvePort(int port) { return port <= 0 ? defaultPort : port; }
    @Override public String toString() { return name + ':' + defaultPort; }
}
