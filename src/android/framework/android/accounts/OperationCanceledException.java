// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.accounts;

/** An account operation canceled. */
public class OperationCanceledException extends android.util.AndroidException {
    public OperationCanceledException() {}
    public OperationCanceledException(String message) { super(message); }
    public OperationCanceledException(String message, Throwable cause) { super(message); initCause(cause); }
    public OperationCanceledException(Exception cause) { super(cause != null ? cause.toString() : null); initCause(cause); }
}
