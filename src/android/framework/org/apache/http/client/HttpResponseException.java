// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.client;

public class HttpResponseException extends ClientProtocolException {
    private final int statusCode;

    public HttpResponseException(int statusCode, String s) {
        super(s);
        this.statusCode = statusCode;
    }
    public int getStatusCode() { return statusCode; }
}
