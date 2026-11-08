// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.cookie;

public class MalformedCookieException extends org.apache.http.ProtocolException {
    public MalformedCookieException() {}
    public MalformedCookieException(String message) { super(message); }
    public MalformedCookieException(String message, Throwable cause) { super(message, cause); }
}
