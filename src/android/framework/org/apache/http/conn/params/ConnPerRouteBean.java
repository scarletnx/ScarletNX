// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.conn.params;

public final class ConnPerRouteBean implements ConnPerRoute {
    public static final int DEFAULT_MAX_CONNECTIONS_PER_ROUTE = 2;
    private int defaultMax;

    public ConnPerRouteBean(int defaultMax) { setDefaultMaxPerRoute(defaultMax); }
    public ConnPerRouteBean() { this(DEFAULT_MAX_CONNECTIONS_PER_ROUTE); }
    public int getDefaultMax() { return defaultMax; }
    public void setDefaultMaxPerRoute(int max) {
        if (max < 1) throw new IllegalArgumentException("The maximum must be greater than 0.");
        defaultMax = max;
    }
    @Override public int getMaxForRoute(Object route) { return defaultMax; }
}
