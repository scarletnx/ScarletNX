// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** A blurred coverage (drawn unblurred here). */
public class BlurMaskFilter extends MaskFilter {
    public enum Blur { NORMAL, SOLID, OUTER, INNER }

    public BlurMaskFilter(float radius, Blur style) {}
}
