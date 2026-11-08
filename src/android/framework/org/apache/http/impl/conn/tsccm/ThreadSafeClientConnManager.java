// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.impl.conn.tsccm;

import java.util.concurrent.TimeUnit;
import org.apache.http.conn.ClientConnectionManager;
import org.apache.http.conn.scheme.SchemeRegistry;
import org.apache.http.params.HttpParams;

/** The connections' manager: the JDK's HttpURLConnection keeps the connections, so it keeps the schemes alone. */
public class ThreadSafeClientConnManager implements ClientConnectionManager {
    protected final SchemeRegistry schemeRegistry;

    public ThreadSafeClientConnManager(HttpParams params, SchemeRegistry schreg) {
        if (params == null) throw new IllegalArgumentException("HTTP parameters may not be null");
        schemeRegistry = schreg;
    }
    @Override public SchemeRegistry getSchemeRegistry() { return schemeRegistry; }
    @Override public void closeIdleConnections(long idletime, TimeUnit tunit) {}
    @Override public void closeExpiredConnections() {}
    @Override public void shutdown() {}
    public int getConnectionsInPool() { return 0; }
}
