// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.impl.client;

import java.io.IOException;
import org.apache.http.client.HttpRequestRetryHandler;
import org.apache.http.protocol.HttpContext;

public class DefaultHttpRequestRetryHandler implements HttpRequestRetryHandler {
    private final int retryCount;
    private final boolean requestSentRetryEnabled;

    public DefaultHttpRequestRetryHandler(int retryCount, boolean requestSentRetryEnabled) {
        this.retryCount = retryCount;
        this.requestSentRetryEnabled = requestSentRetryEnabled;
    }
    public DefaultHttpRequestRetryHandler() { this(3, false); }
    @Override public boolean retryRequest(IOException exception, int executionCount, HttpContext context) {
        if (executionCount > retryCount) return false;
        if (exception instanceof java.io.InterruptedIOException || exception instanceof java.net.UnknownHostException
            || exception instanceof javax.net.ssl.SSLException)
            return false;
        return exception instanceof org.apache.http.NoHttpResponseException || requestSentRetryEnabled;
    }
    public boolean isRequestSentRetryEnabled() { return requestSentRetryEnabled; }
    public int getRetryCount() { return retryCount; }
}
