// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.impl.conn;

import java.util.concurrent.TimeUnit;
import org.apache.http.conn.ClientConnectionManager;
import org.apache.http.conn.scheme.SchemeRegistry;
import org.apache.http.params.HttpParams;

public class SingleClientConnManager implements ClientConnectionManager {
    protected final SchemeRegistry schemeRegistry;

    public SingleClientConnManager(HttpParams params, SchemeRegistry schreg) { schemeRegistry = schreg; }
    @Override public SchemeRegistry getSchemeRegistry() { return schemeRegistry; }
    @Override public void closeIdleConnections(long idletime, TimeUnit tunit) {}
    @Override public void closeExpiredConnections() {}
    @Override public void shutdown() {}
}
