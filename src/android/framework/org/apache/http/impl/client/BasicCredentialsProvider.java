// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.impl.client;

import java.util.HashMap;
import java.util.Map;
import org.apache.http.auth.AuthScope;
import org.apache.http.auth.Credentials;
import org.apache.http.client.CredentialsProvider;

public class BasicCredentialsProvider implements CredentialsProvider {
    private final HashMap<AuthScope, Credentials> credMap = new HashMap<>();

    public BasicCredentialsProvider() {}
    @Override public synchronized void setCredentials(AuthScope authscope, Credentials credentials) {
        if (authscope == null) throw new IllegalArgumentException("Authentication scope may not be null");
        credMap.put(authscope, credentials);
    }
    @Override public synchronized Credentials getCredentials(AuthScope authscope) {
        Credentials best = credMap.get(authscope);
        int bestMatch = -1;
        if (best == null)
            for (Map.Entry<AuthScope, Credentials> e : credMap.entrySet()) {
                int m = authscope.match(e.getKey());
                if (m > bestMatch) {
                    bestMatch = m;
                    best = e.getValue();
                }
            }
        return best;
    }
    @Override public synchronized void clear() { credMap.clear(); }
}
