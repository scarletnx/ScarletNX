// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.message;

import org.apache.http.NameValuePair;

public class BasicNameValuePair implements NameValuePair, Cloneable {
    private final String name, value;

    public BasicNameValuePair(String name, String value) {
        if (name == null) throw new IllegalArgumentException("Name may not be null");
        this.name = name;
        this.value = value;
    }
    @Override public String getName() { return name; }
    @Override public String getValue() { return value; }
    @Override public String toString() { return value == null ? name : name + "=" + value; }
    @Override public boolean equals(Object o) {
        return o instanceof BasicNameValuePair p && name.equals(p.name) && java.util.Objects.equals(value, p.value);
    }
    @Override public int hashCode() { return name.hashCode() * 31 + java.util.Objects.hashCode(value); }
    @Override public Object clone() throws CloneNotSupportedException { return super.clone(); }
}
