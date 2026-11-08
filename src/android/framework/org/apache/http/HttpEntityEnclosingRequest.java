// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http;

public interface HttpEntityEnclosingRequest extends HttpRequest {
    boolean expectContinue();
    void setEntity(HttpEntity entity);
    HttpEntity getEntity();
}
