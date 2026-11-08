// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http;

import org.apache.http.protocol.HttpContext;

public interface HttpRequestInterceptor {
    void process(HttpRequest request, HttpContext context) throws HttpException, java.io.IOException;
}
