// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.conn.scheme;

import java.io.IOException;
import java.net.Socket;

public interface LayeredSocketFactory extends SocketFactory {
    Socket createSocket(Socket socket, String host, int port, boolean autoClose) throws IOException;
}
