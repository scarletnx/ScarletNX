// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.style;

import android.graphics.Typeface;
import android.text.TextPaint;

/** Bold or italic text. */
public class StyleSpan extends MetricAffectingSpan {
    private final int style;

    public StyleSpan(int style) { this.style = style; }
    public int getStyle() { return style; }
    @Override public void updateDrawState(TextPaint ds) { apply(ds); }
    @Override public void updateMeasureState(TextPaint paint) { apply(paint); }

    private void apply(TextPaint p) {
        Typeface old = p.getTypeface();
        int want = (old != null ? old.getStyle() : 0) | style;
        p.setTypeface(Typeface.create(old, want));
    }
}
