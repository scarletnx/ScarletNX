// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.impl.client;

import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import org.apache.http.client.CookieStore;
import org.apache.http.cookie.Cookie;

public class BasicCookieStore implements CookieStore {
    private final ArrayList<Cookie> cookies = new ArrayList<>();

    public BasicCookieStore() {}
    @Override public synchronized void addCookie(Cookie cookie) {
        if (cookie == null) return;
        cookies.removeIf(c -> c.getName().equals(cookie.getName()) && java.util.Objects.equals(c.getDomain(), cookie.getDomain())
                              && java.util.Objects.equals(c.getPath(), cookie.getPath()));
        if (!cookie.isExpired(new Date())) cookies.add(cookie);
    }
    public synchronized void addCookies(Cookie[] cookies) { for (Cookie c : cookies) addCookie(c); }
    @Override public synchronized List<Cookie> getCookies() { return new ArrayList<>(cookies); }
    @Override public synchronized boolean clearExpired(Date date) { return cookies.removeIf(c -> c.isExpired(date)); }
    @Override public synchronized void clear() { cookies.clear(); }
}
