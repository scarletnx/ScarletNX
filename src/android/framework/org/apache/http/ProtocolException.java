// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http;

public class ProtocolException extends HttpException {
    public ProtocolException() {}
    public ProtocolException(String message) { super(message); }
    public ProtocolException(String message, Throwable cause) { super(message, cause); }
}
