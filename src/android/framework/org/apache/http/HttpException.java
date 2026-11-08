// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http;

public class HttpException extends Exception {
    public HttpException() {}
    public HttpException(String message) { super(message); }
    public HttpException(String message, Throwable cause) { super(message, cause); }
}
