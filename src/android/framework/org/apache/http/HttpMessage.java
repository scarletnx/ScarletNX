// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http;

import org.apache.http.params.HttpParams;

public interface HttpMessage {
    ProtocolVersion getProtocolVersion();
    boolean containsHeader(String name);
    Header[] getHeaders(String name);
    Header getFirstHeader(String name);
    Header getLastHeader(String name);
    Header[] getAllHeaders();
    void addHeader(Header header);
    void addHeader(String name, String value);
    void setHeader(Header header);
    void setHeader(String name, String value);
    void setHeaders(Header[] headers);
    void removeHeader(Header header);
    void removeHeaders(String name);
    HeaderIterator headerIterator();
    HeaderIterator headerIterator(String name);
    HttpParams getParams();
    void setParams(HttpParams params);
}
