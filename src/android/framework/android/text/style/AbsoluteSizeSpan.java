// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.style;

import android.text.TextPaint;

/** A text size in pixels, or in dips. */
public class AbsoluteSizeSpan extends MetricAffectingSpan {
    private final int size;
    private final boolean dip;

    public AbsoluteSizeSpan(int size) { this(size, false); }
    public AbsoluteSizeSpan(int size, boolean dip) { this.size = size; this.dip = dip; }
    public int getSize() { return size; }
    public boolean getDip() { return dip; }
    @Override public void updateDrawState(TextPaint ds) { updateMeasureState(ds); }
    @Override public void updateMeasureState(TextPaint ds) { ds.setTextSize(dip ? size * ds.density : size); }
}
