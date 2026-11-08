// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.client.methods;

import java.net.URI;

public class HttpHead extends HttpRequestBase {
    public static final String METHOD_NAME = "HEAD";

    public HttpHead() {}
    public HttpHead(URI uri) { setURI(uri); }
    /** @throws IllegalArgumentException if the uri is invalid */
    public HttpHead(String uri) { setURI(URI.create(uri)); }
    @Override public String getMethod() { return METHOD_NAME; }
}
