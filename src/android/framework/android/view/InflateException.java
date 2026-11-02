// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** A layout that could not be inflated. */
public class InflateException extends RuntimeException {
    public InflateException() {}
    public InflateException(String message) { super(message); }
    public InflateException(String message, Throwable cause) { super(message, cause); }
    public InflateException(Throwable cause) { super(cause); }
}
