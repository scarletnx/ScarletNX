// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

/** A foreground service that may not start now. */
public final class ForegroundServiceStartNotAllowedException extends ServiceStartNotAllowedException {
    public ForegroundServiceStartNotAllowedException(String message) { super(message); }
}
