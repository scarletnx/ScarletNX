// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.message;

import org.apache.http.ProtocolVersion;
import org.apache.http.StatusLine;

public class BasicStatusLine implements StatusLine, Cloneable {
    private final ProtocolVersion protoVersion;
    private final int statusCode;
    private final String reasonPhrase;

    public BasicStatusLine(ProtocolVersion version, int statusCode, String reasonPhrase) {
        if (version == null) throw new IllegalArgumentException("Protocol version may not be null.");
        if (statusCode < 0) throw new IllegalArgumentException("Status code may not be negative.");
        protoVersion = version;
        this.statusCode = statusCode;
        this.reasonPhrase = reasonPhrase;
    }
    @Override public int getStatusCode() { return statusCode; }
    @Override public ProtocolVersion getProtocolVersion() { return protoVersion; }
    @Override public String getReasonPhrase() { return reasonPhrase; }
    @Override public String toString() { return protoVersion + " " + statusCode + (reasonPhrase != null ? " " + reasonPhrase : ""); }
    @Override public Object clone() throws CloneNotSupportedException { return super.clone(); }
}
