// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.params;

public interface CoreConnectionPNames {
    String SO_TIMEOUT = "http.socket.timeout", TCP_NODELAY = "http.tcp.nodelay",
        SOCKET_BUFFER_SIZE = "http.socket.buffer-size", SO_LINGER = "http.socket.linger",
        CONNECTION_TIMEOUT = "http.connection.timeout", STALE_CONNECTION_CHECK = "http.connection.stalecheck";
}
