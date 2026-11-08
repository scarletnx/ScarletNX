// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** A failed binder call. */
public class RemoteException extends android.util.AndroidException {
    public RemoteException() {}
    public RemoteException(String message) { super(message); }
    public RuntimeException rethrowAsRuntimeException() { throw new RuntimeException(this); }
    public RuntimeException rethrowFromSystemServer() { throw new RuntimeException(this); }
}
