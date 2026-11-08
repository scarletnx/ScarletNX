// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.conn.params;

import org.apache.http.params.HttpParams;

public final class ConnManagerParams {
    public static final String TIMEOUT = "http.conn-manager.timeout", MAX_CONNECTIONS_PER_ROUTE = "http.conn-manager.max-per-route",
        MAX_TOTAL_CONNECTIONS = "http.conn-manager.max-total";
    public static final int DEFAULT_MAX_TOTAL_CONNECTIONS = 20;

    private ConnManagerParams() {}
    public static long getTimeout(HttpParams params) { return params.getLongParameter(TIMEOUT, 0); }
    public static void setTimeout(HttpParams params, long timeout) { params.setLongParameter(TIMEOUT, timeout); }
    public static void setMaxConnectionsPerRoute(HttpParams params, ConnPerRoute connPerRoute) {
        params.setParameter(MAX_CONNECTIONS_PER_ROUTE, connPerRoute);
    }
    public static ConnPerRoute getMaxConnectionsPerRoute(HttpParams params) {
        return params.getParameter(MAX_CONNECTIONS_PER_ROUTE) instanceof ConnPerRoute c ? c : new ConnPerRouteBean();
    }
    public static void setMaxTotalConnections(HttpParams params, int maxTotalConnections) {
        params.setIntParameter(MAX_TOTAL_CONNECTIONS, maxTotalConnections);
    }
    public static int getMaxTotalConnections(HttpParams params) {
        return params.getIntParameter(MAX_TOTAL_CONNECTIONS, DEFAULT_MAX_TOTAL_CONNECTIONS);
    }
}
