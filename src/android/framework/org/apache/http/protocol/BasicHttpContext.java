// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.protocol;

import java.util.HashMap;

public class BasicHttpContext implements HttpContext {
    private final HttpContext parentContext;
    private final HashMap<String, Object> map = new HashMap<>();

    public BasicHttpContext() { this(null); }
    public BasicHttpContext(HttpContext parentContext) { this.parentContext = parentContext; }
    @Override public Object getAttribute(String id) {
        if (id == null) throw new IllegalArgumentException("Id may not be null");
        Object o = map.get(id);
        return o == null && parentContext != null ? parentContext.getAttribute(id) : o;
    }
    @Override public void setAttribute(String id, Object obj) {
        if (id == null) throw new IllegalArgumentException("Id may not be null");
        map.put(id, obj);
    }
    @Override public Object removeAttribute(String id) { return map.remove(id); }
}
