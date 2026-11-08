// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.conn.scheme;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import org.apache.http.HttpHost;

public final class SchemeRegistry {
    private final LinkedHashMap<String, Scheme> registeredSchemes = new LinkedHashMap<>();

    public SchemeRegistry() {}
    public synchronized Scheme getScheme(String name) {
        Scheme s = get(name);
        if (s == null) throw new IllegalStateException("Scheme '" + name + "' not registered.");
        return s;
    }
    public synchronized Scheme getScheme(HttpHost host) { return getScheme(host.getSchemeName()); }
    public synchronized Scheme get(String name) { return registeredSchemes.get(name); }
    public synchronized Scheme register(Scheme sch) { return registeredSchemes.put(sch.getName(), sch); }
    public synchronized Scheme unregister(String name) { return registeredSchemes.remove(name); }
    public synchronized List<String> getSchemeNames() { return new ArrayList<>(registeredSchemes.keySet()); }
}
