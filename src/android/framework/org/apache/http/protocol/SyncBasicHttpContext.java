// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.protocol;

public class SyncBasicHttpContext extends BasicHttpContext {
    public SyncBasicHttpContext(HttpContext parentContext) { super(parentContext); }
    @Override public synchronized Object getAttribute(String id) { return super.getAttribute(id); }
    @Override public synchronized void setAttribute(String id, Object obj) { super.setAttribute(id, obj); }
    @Override public synchronized Object removeAttribute(String id) { return super.removeAttribute(id); }
}
