// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.client;

public class ClientProtocolException extends java.io.IOException {
    public ClientProtocolException() {}
    public ClientProtocolException(String s) { super(s); }
    public ClientProtocolException(Throwable cause) { super(cause); }
    public ClientProtocolException(String message, Throwable cause) { super(message, cause); }
}
