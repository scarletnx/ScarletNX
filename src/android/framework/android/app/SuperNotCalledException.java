// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

/** A lifecycle method that did not call its super. */
public final class SuperNotCalledException extends android.util.AndroidRuntimeException {
    public SuperNotCalledException(String msg) { super(msg); }
}
