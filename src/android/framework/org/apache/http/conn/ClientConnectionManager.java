// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.conn;

import java.util.concurrent.TimeUnit;
import org.apache.http.conn.scheme.SchemeRegistry;

public interface ClientConnectionManager {
    SchemeRegistry getSchemeRegistry();
    void closeIdleConnections(long idletime, TimeUnit tunit);
    void closeExpiredConnections();
    void shutdown();
}
