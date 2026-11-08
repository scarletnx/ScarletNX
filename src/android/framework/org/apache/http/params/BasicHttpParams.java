// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.params;

import java.util.HashMap;

public final class BasicHttpParams extends AbstractHttpParams implements java.io.Serializable, Cloneable {
    private final HashMap<String, Object> parameters = new HashMap<>();

    public BasicHttpParams() {}
    @Override public synchronized Object getParameter(String name) { return parameters.get(name); }
    @Override public synchronized HttpParams setParameter(String name, Object value) {
        if (value != null) parameters.put(name, value);
        else parameters.remove(name);
        return this;
    }
    @Override public synchronized boolean removeParameter(String name) { return parameters.remove(name) != null; }
    public synchronized void setParameters(String[] names, Object value) { for (String n : names) setParameter(n, value); }
    public synchronized boolean isParameterSet(String name) { return parameters.containsKey(name); }
    public synchronized boolean isParameterSetLocally(String name) { return parameters.containsKey(name); }
    public synchronized void clear() { parameters.clear(); }
    @Override public synchronized HttpParams copy() {
        BasicHttpParams c = new BasicHttpParams();
        c.parameters.putAll(parameters);
        return c;
    }
    @Override public Object clone() throws CloneNotSupportedException { return copy(); }
}
