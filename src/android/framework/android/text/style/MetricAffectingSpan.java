// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.style;

import android.text.TextPaint;

/** A span that changes how its characters measure. */
public abstract class MetricAffectingSpan extends CharacterStyle implements UpdateLayout {
    public abstract void updateMeasureState(TextPaint textPaint);

    @Override public MetricAffectingSpan getUnderlying() { return this; }
}
