// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.message;

import org.apache.http.Header;
import org.apache.http.HeaderElement;

public class BasicHeader implements Header, Cloneable {
    private final String name, value;

    public BasicHeader(String name, String value) {
        if (name == null) throw new IllegalArgumentException("Name may not be null");
        this.name = name;
        this.value = value;
    }
    @Override public String getName() { return name; }
    @Override public String getValue() { return value; }
    @Override public HeaderElement[] getElements() { return BasicHeaderElement.parse(value); }
    @Override public String toString() { return name + ": " + (value != null ? value : ""); }
    @Override public Object clone() throws CloneNotSupportedException { return super.clone(); }
}
