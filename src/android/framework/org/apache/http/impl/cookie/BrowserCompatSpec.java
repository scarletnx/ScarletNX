// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.impl.cookie;

import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import org.apache.http.Header;
import org.apache.http.HeaderElement;
import org.apache.http.NameValuePair;
import org.apache.http.cookie.Cookie;
import org.apache.http.cookie.CookieOrigin;
import org.apache.http.cookie.CookieSpec;
import org.apache.http.cookie.MalformedCookieException;
import org.apache.http.message.BasicHeader;

/** Cookies as browsers take them: Set-Cookie's name=value with its domain, path, expires, max-age and secure. */
public class BrowserCompatSpec implements CookieSpec {
    public BrowserCompatSpec(String[] datepatterns) {}
    public BrowserCompatSpec() {}

    @Override public int getVersion() { return 0; }
    @Override public List<Cookie> parse(Header header, CookieOrigin origin) throws MalformedCookieException {
        if (header == null || origin == null) throw new IllegalArgumentException("Header and origin may not be null");
        List<Cookie> out = new ArrayList<>();
        String value = header.getValue();
        if (value == null) return out;
        String[] parts = value.split(";");
        int eq = parts[0].indexOf('=');
        if (eq <= 0) throw new MalformedCookieException("Cookie name may not be empty");
        BasicClientCookie cookie = new BasicClientCookie(parts[0].substring(0, eq).trim(), parts[0].substring(eq + 1).trim());
        String path = origin.getPath();
        int slash = path.lastIndexOf('/');
        cookie.setPath(slash > 0 ? path.substring(0, slash) : "/");
        cookie.setDomain(origin.getHost());
        for (int i = 1; i < parts.length; ++i) {
            String p = parts[i].trim();
            int e = p.indexOf('=');
            String name = (e >= 0 ? p.substring(0, e) : p).trim().toLowerCase(java.util.Locale.ENGLISH);
            String v = e >= 0 ? p.substring(e + 1).trim() : null;
            switch (name) {
            case "domain" -> { if (v != null && !v.isEmpty()) cookie.setDomain(v); }
            case "path" -> { if (v != null && !v.isEmpty()) cookie.setPath(v); }
            case "secure" -> cookie.setSecure(true);
            case "comment" -> cookie.setComment(v);
            case "max-age" -> {
                try {
                    cookie.setExpiryDate(new Date(System.currentTimeMillis() + Long.parseLong(v) * 1000));
                } catch (NumberFormatException ex) {
                    // ignored, as browsers do
                }
            }
            case "expires" -> {
                try {
                    if (v != null) cookie.setExpiryDate(DateUtils.parseDate(v));
                } catch (DateParseException ex) {
                    // ignored, as browsers do
                }
            }
            case "version" -> {
                try {
                    cookie.setVersion(Integer.parseInt(v));
                } catch (NumberFormatException ex) {
                    // ignored
                }
            }
            default -> {}
            }
            cookie.setAttribute(name, v);
        }
        out.add(cookie);
        return out;
    }
    @Override public void validate(Cookie cookie, CookieOrigin origin) throws MalformedCookieException {
        if (cookie == null || origin == null) throw new IllegalArgumentException("Cookie and origin may not be null");
    }
    @Override public boolean match(Cookie cookie, CookieOrigin origin) {
        String domain = cookie.getDomain();
        String host = origin.getHost();
        if (domain != null) {
            String d = domain.startsWith(".") ? domain.substring(1) : domain;
            if (!host.equals(d) && !host.endsWith("." + d)) return false;
        }
        String path = cookie.getPath() != null ? cookie.getPath() : "/";
        if (!origin.getPath().startsWith(path)) return false;
        return !cookie.isSecure() || origin.isSecure();
    }
    @Override public List<Header> formatCookies(List<Cookie> cookies) {
        StringBuilder b = new StringBuilder();
        for (Cookie c : cookies) {
            if (b.length() > 0) b.append("; ");
            b.append(c.getName()).append('=').append(c.getValue() != null ? c.getValue() : "");
        }
        List<Header> out = new ArrayList<>();
        out.add(new BasicHeader("Cookie", b.toString()));
        return out;
    }
    @Override public Header getVersionHeader() { return null; }
    @Override public String toString() { return "compatibility"; }
}
