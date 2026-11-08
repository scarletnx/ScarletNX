// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.client.methods;

import org.apache.http.Header;
import org.apache.http.HttpEntity;
import org.apache.http.HttpEntityEnclosingRequest;

public abstract class HttpEntityEnclosingRequestBase extends HttpRequestBase implements HttpEntityEnclosingRequest {
    private HttpEntity entity;

    public HttpEntityEnclosingRequestBase() {}
    @Override public HttpEntity getEntity() { return entity; }
    @Override public void setEntity(HttpEntity entity) { this.entity = entity; }
    @Override public boolean expectContinue() {
        Header expect = getFirstHeader("Expect");
        return expect != null && "100-continue".equalsIgnoreCase(expect.getValue());
    }
}
