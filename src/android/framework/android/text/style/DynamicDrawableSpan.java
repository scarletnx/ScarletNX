// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.text.style;

import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;

/** A span drawn as a drawable, aligned to the bottom, the baseline or the center of its line. */
public abstract class DynamicDrawableSpan extends ReplacementSpan {
    public static final int ALIGN_BOTTOM = 0, ALIGN_BASELINE = 1, ALIGN_CENTER = 2;
    protected final int mVerticalAlignment;

    public DynamicDrawableSpan() { mVerticalAlignment = ALIGN_BOTTOM; }
    protected DynamicDrawableSpan(int verticalAlignment) { mVerticalAlignment = verticalAlignment; }
    public int getVerticalAlignment() { return mVerticalAlignment; }

    public abstract Drawable getDrawable();

    @Override public int getSize(Paint paint, CharSequence text, int start, int end, Paint.FontMetricsInt fm) {
        Rect rect = getDrawable().getBounds();
        if (fm != null) {
            fm.ascent = -rect.bottom;
            fm.descent = 0;
            fm.top = fm.ascent;
            fm.bottom = 0;
        }
        return rect.right;
    }
    @Override public void draw(Canvas canvas, CharSequence text, int start, int end, float x, int top, int y, int bottom, Paint paint) {
        Drawable b = getDrawable();
        int h = b.getBounds().height();
        int transY = bottom - b.getBounds().bottom;
        if (mVerticalAlignment == ALIGN_BASELINE) transY -= paint.getFontMetricsInt().descent;
        else if (mVerticalAlignment == ALIGN_CENTER) transY = top + (bottom - top) / 2 - h / 2;
        int save = canvas.save();
        canvas.translate(x, transY);
        b.draw(canvas);
        canvas.restoreToCount(save);
    }
}
