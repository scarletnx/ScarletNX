// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.client;

import java.io.IOException;
import org.apache.http.protocol.HttpContext;

public interface HttpRequestRetryHandler {
    boolean retryRequest(IOException exception, int executionCount, HttpContext context);
}
