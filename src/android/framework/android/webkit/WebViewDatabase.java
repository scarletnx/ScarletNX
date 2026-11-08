// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

import java.util.HashMap;

/** The WebView's saved HTTP authentication, by host and realm, for the process */
public class WebViewDatabase {
    private static final WebViewDatabase INSTANCE = new WebViewDatabase();
    private final HashMap<String, String[]> auth = new HashMap<>();

    public static WebViewDatabase getInstance(android.content.Context context) { return INSTANCE; }
    @Deprecated public boolean hasUsernamePassword() { return false; }
    @Deprecated public void clearUsernamePassword() {}
    public synchronized boolean hasHttpAuthUsernamePassword() { return !auth.isEmpty(); }
    public synchronized void clearHttpAuthUsernamePassword() { auth.clear(); }
    public synchronized void setHttpAuthUsernamePassword(String host, String realm, String username, String password) {
        auth.put(host + "\n" + realm, new String[] {username, password});
    }
    public synchronized String[] getHttpAuthUsernamePassword(String host, String realm) {
        String[] v = auth.get(host + "\n" + realm);
        return v != null ? v.clone() : null;
    }
    public boolean hasFormData() { return false; }
    public void clearFormData() {}
}
