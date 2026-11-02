// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.style;

import android.graphics.Typeface;
import android.text.TextPaint;

/** A font family or typeface for text. */
public class TypefaceSpan extends MetricAffectingSpan {
    private final String family;
    private final Typeface typeface;

    public TypefaceSpan(String family) { this.family = family; typeface = null; }
    public TypefaceSpan(Typeface typeface) { family = null; this.typeface = typeface; }
    public String getFamily() { return family; }
    public Typeface getTypeface() { return typeface; }
    @Override public void updateDrawState(TextPaint ds) { apply(ds); }
    @Override public void updateMeasureState(TextPaint paint) { apply(paint); }

    private void apply(TextPaint p) {
        Typeface old = p.getTypeface();
        int style = old != null ? old.getStyle() : 0;
        p.setTypeface(typeface != null ? Typeface.create(typeface, style) : Typeface.create(family, style));
    }
}
