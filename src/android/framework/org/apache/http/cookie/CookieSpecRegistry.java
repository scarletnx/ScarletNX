// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.cookie;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import org.apache.http.params.HttpParams;

public final class CookieSpecRegistry {
    private final LinkedHashMap<String, CookieSpecFactory> registeredSpecs = new LinkedHashMap<>();

    public CookieSpecRegistry() {}
    public synchronized void register(String name, CookieSpecFactory factory) {
        if (name == null) throw new IllegalArgumentException("Name may not be null");
        if (factory == null) throw new IllegalArgumentException("Cookie spec factory may not be null");
        registeredSpecs.put(name.toLowerCase(Locale.ENGLISH), factory);
    }
    public synchronized void unregister(String id) { registeredSpecs.remove(id.toLowerCase(Locale.ENGLISH)); }
    public synchronized CookieSpec getCookieSpec(String name, HttpParams params) throws IllegalStateException {
        CookieSpecFactory factory = registeredSpecs.get(name.toLowerCase(Locale.ENGLISH));
        if (factory == null) throw new IllegalStateException("Unsupported cookie spec: " + name);
        return factory.newInstance(params);
    }
    public synchronized CookieSpec getCookieSpec(String name) { return getCookieSpec(name, null); }
    public synchronized List<String> getSpecNames() { return new ArrayList<>(registeredSpecs.keySet()); }
}
