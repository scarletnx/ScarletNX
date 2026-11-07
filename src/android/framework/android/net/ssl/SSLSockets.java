// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.net.ssl;

import javax.net.ssl.SSLSocket;

/** Options of TLS sockets. The JDK's sockets take each socket; they use session tickets by default. */
public class SSLSockets {
    private SSLSockets() {}

    public static boolean isSupportedSocket(SSLSocket socket) { return socket != null; }

    public static void setUseSessionTickets(SSLSocket socket, boolean useSessionTickets) {
        if (!isSupportedSocket(socket)) throw new IllegalArgumentException("Socket is not a supported socket.");
    }
}
