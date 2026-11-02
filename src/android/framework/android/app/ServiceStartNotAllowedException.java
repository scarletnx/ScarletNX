// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

/** A service that may not start now. */
public abstract class ServiceStartNotAllowedException extends IllegalStateException {
    ServiceStartNotAllowedException(String message) { super(message); }
}
