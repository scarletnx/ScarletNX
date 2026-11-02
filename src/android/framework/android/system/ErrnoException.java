// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.system;

/** A failed system call. */
public final class ErrnoException extends Exception {
    public final int errno;
    private final String functionName;
    public ErrnoException(String functionName, int errno) { super(functionName + " failed: " + errno); this.functionName = functionName; this.errno = errno; }
    public ErrnoException(String functionName, int errno, Throwable cause) { this(functionName, errno); initCause(cause); }
}
