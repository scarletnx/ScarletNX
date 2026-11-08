// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.conn.scheme;

import java.io.IOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import org.apache.http.params.HttpConnectionParams;
import org.apache.http.params.HttpParams;

public final class PlainSocketFactory implements SocketFactory {
    private static final PlainSocketFactory DEFAULT_FACTORY = new PlainSocketFactory();

    public PlainSocketFactory() {}
    public static PlainSocketFactory getSocketFactory() { return DEFAULT_FACTORY; }
    @Override public Socket createSocket() { return new Socket(); }
    @Override public Socket connectSocket(Socket sock, String host, int port, InetAddress localAddress, int localPort,
                                          HttpParams params) throws IOException {
        if (sock == null) sock = createSocket();
        if (localAddress != null || localPort > 0) sock.bind(new InetSocketAddress(localAddress, Math.max(localPort, 0)));
        sock.connect(new InetSocketAddress(host, port), params != null ? HttpConnectionParams.getConnectionTimeout(params) : 0);
        return sock;
    }
    @Override public boolean isSecure(Socket sock) { return false; }
}
