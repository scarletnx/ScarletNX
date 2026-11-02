// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** An operation canceled by a CancellationSignal. */
public class OperationCanceledException extends RuntimeException {
    public OperationCanceledException() { this(null); }
    public OperationCanceledException(String message) { super(message != null ? message : "The operation has been canceled."); }
}
