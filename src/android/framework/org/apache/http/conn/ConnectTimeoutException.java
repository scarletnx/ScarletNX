// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.conn;

public class ConnectTimeoutException extends java.io.InterruptedIOException {
    public ConnectTimeoutException() {}
    public ConnectTimeoutException(String message) { super(message); }
}
