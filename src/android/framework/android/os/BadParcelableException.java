// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** A Parcelable that can't be read. */
public class BadParcelableException extends android.util.AndroidRuntimeException {
    public BadParcelableException() {}
    public BadParcelableException(String message) { super(message); }
    public BadParcelableException(String message, Throwable cause) { super(message); initCause(cause); }
    public BadParcelableException(Exception cause) { super(cause != null ? cause.toString() : null); initCause(cause); }
}
