// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.impl.cookie;

import java.util.Date;
import java.util.HashMap;
import org.apache.http.cookie.SetCookie;

public class BasicClientCookie implements SetCookie, Cloneable, java.io.Serializable {
    private final String name;
    private final HashMap<String, String> attribs = new HashMap<>();
    private String value, cookieComment, cookieDomain, cookiePath;
    private Date cookieExpiryDate;
    private boolean isSecure;
    private int cookieVersion;

    public BasicClientCookie(String name, String value) {
        if (name == null) throw new IllegalArgumentException("Name may not be null");
        this.name = name;
        this.value = value;
    }
    @Override public String getName() { return name; }
    @Override public String getValue() { return value; }
    @Override public void setValue(String value) { this.value = value; }
    @Override public String getComment() { return cookieComment; }
    @Override public void setComment(String comment) { cookieComment = comment; }
    @Override public String getCommentURL() { return null; }
    @Override public Date getExpiryDate() { return cookieExpiryDate; }
    @Override public void setExpiryDate(Date expiryDate) { cookieExpiryDate = expiryDate; }
    @Override public boolean isPersistent() { return cookieExpiryDate != null; }
    @Override public String getDomain() { return cookieDomain; }
    @Override public void setDomain(String domain) { cookieDomain = domain != null ? domain.toLowerCase(java.util.Locale.ENGLISH) : null; }
    @Override public String getPath() { return cookiePath; }
    @Override public void setPath(String path) { cookiePath = path; }
    @Override public boolean isSecure() { return isSecure; }
    @Override public void setSecure(boolean secure) { isSecure = secure; }
    @Override public int[] getPorts() { return null; }
    @Override public int getVersion() { return cookieVersion; }
    @Override public void setVersion(int version) { cookieVersion = version; }
    @Override public boolean isExpired(Date date) {
        if (date == null) throw new IllegalArgumentException("Date may not be null");
        return cookieExpiryDate != null && cookieExpiryDate.getTime() <= date.getTime();
    }
    public void setAttribute(String name, String value) { attribs.put(name, value); }
    public String getAttribute(String name) { return attribs.get(name); }
    public boolean containsAttribute(String name) { return attribs.get(name) != null; }
    @Override public Object clone() throws CloneNotSupportedException { return super.clone(); }
    @Override public String toString() { return "[version: " + cookieVersion + "][name: " + name + "][value: " + value + "]"; }
}
