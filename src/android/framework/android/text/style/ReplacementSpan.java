// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.style;

import android.graphics.Canvas;
import android.graphics.Paint;
import android.text.TextPaint;

/** A span drawn in place of its characters. */
public abstract class ReplacementSpan extends MetricAffectingSpan {
    private CharSequence contentDescription;

    public abstract int getSize(Paint paint, CharSequence text, int start, int end, Paint.FontMetricsInt fm);

    public abstract void draw(Canvas canvas, CharSequence text, int start, int end, float x, int top, int y, int bottom, Paint paint);

    public CharSequence getContentDescription() { return contentDescription; }
    public void setContentDescription(CharSequence d) { contentDescription = d; }
    @Override public void updateMeasureState(TextPaint p) {}
    @Override public void updateDrawState(TextPaint ds) {}
}
