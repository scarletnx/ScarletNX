// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http;

/** One comma-separated element of a header's value: name=value; params. */
public interface HeaderElement {
    String getName();
    String getValue();
    NameValuePair[] getParameters();
    NameValuePair getParameterByName(String name);
    int getParameterCount();
    NameValuePair getParameter(int index);
}
