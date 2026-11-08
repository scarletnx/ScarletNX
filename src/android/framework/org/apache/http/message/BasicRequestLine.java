// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.message;

import org.apache.http.ProtocolVersion;
import org.apache.http.RequestLine;

public class BasicRequestLine implements RequestLine, Cloneable {
    private final ProtocolVersion protoversion;
    private final String method, uri;

    public BasicRequestLine(String method, String uri, ProtocolVersion version) {
        this.method = method;
        this.uri = uri;
        this.protoversion = version;
    }
    @Override public String getMethod() { return method; }
    @Override public ProtocolVersion getProtocolVersion() { return protoversion; }
    @Override public String getUri() { return uri; }
    @Override public String toString() { return method + " " + uri + " " + protoversion; }
    @Override public Object clone() throws CloneNotSupportedException { return super.clone(); }
}
