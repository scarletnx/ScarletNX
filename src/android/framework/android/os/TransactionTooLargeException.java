// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** A binder transaction too large. */
public class TransactionTooLargeException extends android.os.RemoteException {
    public TransactionTooLargeException() {}
    public TransactionTooLargeException(String message) { super(message); }
    public TransactionTooLargeException(String message, Throwable cause) { super(message); initCause(cause); }
    public TransactionTooLargeException(Exception cause) { super(cause != null ? cause.toString() : null); initCause(cause); }
}
