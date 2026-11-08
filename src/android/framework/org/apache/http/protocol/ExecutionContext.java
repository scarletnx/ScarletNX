// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.protocol;

public interface ExecutionContext {
    String HTTP_CONNECTION = "http.connection", HTTP_REQUEST = "http.request", HTTP_RESPONSE = "http.response",
        HTTP_TARGET_HOST = "http.target_host", HTTP_PROXY_HOST = "http.proxy_host", HTTP_REQ_SENT = "http.request_sent";
}
