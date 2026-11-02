// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

/** The base of the framework's checked exceptions. */
public class AndroidException extends Exception {
    public AndroidException() {}
    public AndroidException(String name) { super(name); }
    public AndroidException(String name, Throwable cause) { super(name, cause); }
    public AndroidException(Exception cause) { super(cause); }
}
