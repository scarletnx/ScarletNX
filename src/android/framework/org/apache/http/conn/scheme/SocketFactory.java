// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.conn.scheme;

import java.io.IOException;
import java.net.InetAddress;
import java.net.Socket;
import org.apache.http.params.HttpParams;

public interface SocketFactory {
    Socket createSocket() throws IOException;
    Socket connectSocket(Socket sock, String host, int port, InetAddress localAddress, int localPort, HttpParams params)
        throws IOException;
    boolean isSecure(Socket sock) throws IllegalArgumentException;
}
