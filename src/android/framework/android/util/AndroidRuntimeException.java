// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

/** The base of the framework's unchecked exceptions. */
public class AndroidRuntimeException extends RuntimeException {
    public AndroidRuntimeException() {}
    public AndroidRuntimeException(String name) { super(name); }
    public AndroidRuntimeException(String name, Throwable cause) { super(name, cause); }
    public AndroidRuntimeException(Exception cause) { super(cause); }
}
