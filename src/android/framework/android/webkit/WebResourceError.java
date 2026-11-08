// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

public abstract class WebResourceError {
    public abstract int getErrorCode();
    public abstract CharSequence getDescription();
}
