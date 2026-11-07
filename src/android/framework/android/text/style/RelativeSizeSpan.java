// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.style;

import android.text.TextPaint;

/** A text size relative to the paint's. */
public class RelativeSizeSpan extends MetricAffectingSpan {
    private final float proportion;

    public RelativeSizeSpan(float proportion) { this.proportion = proportion; }
    public float getSizeChange() { return proportion; }
    @Override public void updateDrawState(TextPaint ds) { updateMeasureState(ds); }
    @Override public void updateMeasureState(TextPaint ds) { ds.setTextSize(ds.getTextSize() * proportion); }
}
