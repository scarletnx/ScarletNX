// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2018 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
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
