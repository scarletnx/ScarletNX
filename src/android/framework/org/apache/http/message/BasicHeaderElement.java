// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.message;

import java.util.ArrayList;
import org.apache.http.HeaderElement;
import org.apache.http.NameValuePair;

public class BasicHeaderElement implements HeaderElement, Cloneable {
    private final String name, value;
    private final NameValuePair[] parameters;

    public BasicHeaderElement(String name, String value, NameValuePair[] parameters) {
        if (name == null) throw new IllegalArgumentException("Name may not be null");
        this.name = name;
        this.value = value;
        this.parameters = parameters != null ? parameters : new NameValuePair[0];
    }
    public BasicHeaderElement(String name, String value) { this(name, value, null); }

    /** a header's value cut into its elements: a, b=c; d=e */
    static HeaderElement[] parse(String value) {
        if (value == null) return new HeaderElement[0];
        ArrayList<HeaderElement> out = new ArrayList<>();
        for (String element : value.split(",")) {
            String[] parts = element.split(";");
            NameValuePair head = pair(parts[0]);
            if (head == null) continue;
            ArrayList<NameValuePair> params = new ArrayList<>();
            for (int i = 1; i < parts.length; ++i) {
                NameValuePair p = pair(parts[i]);
                if (p != null) params.add(p);
            }
            out.add(new BasicHeaderElement(head.getName(), head.getValue(), params.toArray(new NameValuePair[0])));
        }
        return out.toArray(new HeaderElement[0]);
    }
    private static NameValuePair pair(String text) {
        String t = text.trim();
        if (t.isEmpty()) return null;
        int eq = t.indexOf('=');
        if (eq < 0) return new BasicNameValuePair(t, null);
        String v = t.substring(eq + 1).trim();
        if (v.length() >= 2 && v.startsWith("\"") && v.endsWith("\"")) v = v.substring(1, v.length() - 1);
        return new BasicNameValuePair(t.substring(0, eq).trim(), v);
    }

    @Override public String getName() { return name; }
    @Override public String getValue() { return value; }
    @Override public NameValuePair[] getParameters() { return parameters.clone(); }
    @Override public int getParameterCount() { return parameters.length; }
    @Override public NameValuePair getParameter(int index) { return parameters[index]; }
    @Override public NameValuePair getParameterByName(String n) {
        for (NameValuePair p : parameters)
            if (p.getName().equalsIgnoreCase(n)) return p;
        return null;
    }
    @Override public Object clone() throws CloneNotSupportedException { return super.clone(); }
}
