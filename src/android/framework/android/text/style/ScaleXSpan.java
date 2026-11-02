// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.style;

import android.text.TextPaint;

/** A horizontal scale of text. */
public class ScaleXSpan extends MetricAffectingSpan {
    private final float proportion;

    public ScaleXSpan(float proportion) { this.proportion = proportion; }
    public float getScaleX() { return proportion; }
    @Override public void updateDrawState(TextPaint ds) { updateMeasureState(ds); }
    @Override public void updateMeasureState(TextPaint ds) { ds.setTextScaleX(ds.getTextScaleX() * proportion); }
}
