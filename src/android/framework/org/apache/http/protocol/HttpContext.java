// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.protocol;

public interface HttpContext {
    String RESERVED_PREFIX = "http.";
    Object getAttribute(String id);
    void setAttribute(String id, Object obj);
    Object removeAttribute(String id);
}
