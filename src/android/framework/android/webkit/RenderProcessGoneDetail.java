// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.webkit;

public abstract class RenderProcessGoneDetail {
    public abstract boolean didCrash();
    public abstract int rendererPriorityAtExit();
}
