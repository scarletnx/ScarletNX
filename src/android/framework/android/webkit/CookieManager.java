// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

import java.util.LinkedHashMap;
import java.util.Map;

/** The process's cookies for its WebViews, kept in memory by the URL's host */
public class CookieManager {
    private static final CookieManager instance = new CookieManager();
    private final Map<String, LinkedHashMap<String, String>> cookies = new LinkedHashMap<>();
    private boolean accept = true;

    protected CookieManager() {}
    public static CookieManager getInstance() { return instance; }

    private static String host(String url) {
        try {
            String h = java.net.URI.create(url).getHost();
            return h != null ? h : url;
        } catch (IllegalArgumentException e) {
            return url;
        }
    }
    public synchronized void setAcceptCookie(boolean accept) { this.accept = accept; }
    public synchronized boolean acceptCookie() { return accept; }
    public void setAcceptThirdPartyCookies(WebView webview, boolean accept) {}
    public boolean acceptThirdPartyCookies(WebView webview) { return false; }
    public synchronized void setCookie(String url, String value) {
        int eq = value.indexOf('='), semi = value.indexOf(';');
        if (eq <= 0) return;
        String name = value.substring(0, eq).trim(), v = value.substring(eq + 1, semi > eq ? semi : value.length()).trim();
        cookies.computeIfAbsent(host(url), k -> new LinkedHashMap<>()).put(name, v);
    }
    public void setCookie(String url, String value, ValueCallback<Boolean> callback) {
        setCookie(url, value);
        if (callback != null) callback.onReceiveValue(true);
    }
    public synchronized String getCookie(String url) {
        LinkedHashMap<String, String> m = cookies.get(host(url));
        if (m == null || m.isEmpty()) return null;
        StringBuilder b = new StringBuilder();
        for (Map.Entry<String, String> e : m.entrySet()) {
            if (b.length() > 0) b.append("; ");
            b.append(e.getKey()).append('=').append(e.getValue());
        }
        return b.toString();
    }
    @Deprecated public synchronized void removeAllCookie() { cookies.clear(); }
    public void removeAllCookies(ValueCallback<Boolean> callback) {
        synchronized (this) { cookies.clear(); }
        if (callback != null) callback.onReceiveValue(true);
    }
    @Deprecated public void removeSessionCookie() {}
    public void removeSessionCookies(ValueCallback<Boolean> callback) { if (callback != null) callback.onReceiveValue(true); }
    @Deprecated public void removeExpiredCookie() {}
    public synchronized boolean hasCookies() { return !cookies.isEmpty(); }
    public void flush() {}
}
