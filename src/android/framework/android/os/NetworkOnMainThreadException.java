// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** Network use on the main thread, which StrictMode forbids. */
public class NetworkOnMainThreadException extends android.util.AndroidRuntimeException {
    public NetworkOnMainThreadException() {}
    public NetworkOnMainThreadException(String message) { super(message); }
    public NetworkOnMainThreadException(String message, Throwable cause) { super(message); initCause(cause); }
    public NetworkOnMainThreadException(Exception cause) { super(cause != null ? cause.toString() : null); initCause(cause); }
}
