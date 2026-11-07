// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** A binder whose process died. */
public class DeadObjectException extends RemoteException {
    public DeadObjectException() {}
    public DeadObjectException(String message) { super(message); }
}
