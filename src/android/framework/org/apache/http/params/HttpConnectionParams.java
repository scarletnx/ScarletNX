// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.params;

public final class HttpConnectionParams implements CoreConnectionPNames {
    private HttpConnectionParams() {}
    public static int getSoTimeout(HttpParams params) { return params.getIntParameter(SO_TIMEOUT, 0); }
    public static void setSoTimeout(HttpParams params, int timeout) { params.setIntParameter(SO_TIMEOUT, timeout); }
    public static boolean getTcpNoDelay(HttpParams params) { return params.getBooleanParameter(TCP_NODELAY, true); }
    public static void setTcpNoDelay(HttpParams params, boolean value) { params.setBooleanParameter(TCP_NODELAY, value); }
    public static int getSocketBufferSize(HttpParams params) { return params.getIntParameter(SOCKET_BUFFER_SIZE, -1); }
    public static void setSocketBufferSize(HttpParams params, int size) { params.setIntParameter(SOCKET_BUFFER_SIZE, size); }
    public static int getLinger(HttpParams params) { return params.getIntParameter(SO_LINGER, -1); }
    public static void setLinger(HttpParams params, int value) { params.setIntParameter(SO_LINGER, value); }
    public static int getConnectionTimeout(HttpParams params) { return params.getIntParameter(CONNECTION_TIMEOUT, 0); }
    public static void setConnectionTimeout(HttpParams params, int timeout) { params.setIntParameter(CONNECTION_TIMEOUT, timeout); }
    public static boolean isStaleCheckingEnabled(HttpParams params) { return params.getBooleanParameter(STALE_CONNECTION_CHECK, true); }
    public static void setStaleCheckingEnabled(HttpParams params, boolean value) { params.setBooleanParameter(STALE_CONNECTION_CHECK, value); }
}
