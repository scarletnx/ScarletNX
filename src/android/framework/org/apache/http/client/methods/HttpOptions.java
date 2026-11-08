// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.client.methods;

import java.net.URI;

public class HttpOptions extends HttpRequestBase {
    public static final String METHOD_NAME = "OPTIONS";

    public HttpOptions() {}
    public HttpOptions(URI uri) { setURI(uri); }
    /** @throws IllegalArgumentException if the uri is invalid */
    public HttpOptions(String uri) { setURI(URI.create(uri)); }
    @Override public String getMethod() { return METHOD_NAME; }
}
