// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.graphics.Canvas;
import android.util.AttributeSet;
import android.view.View;

/** An empty view for gaps in a layout: invisible, it draws nothing, and it measures to its minimum size, as Android's. */
public final class Space extends View {
    public Space(Context context) { this(context, null); }
    public Space(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public Space(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public Space(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        if (getVisibility() == VISIBLE) setVisibility(INVISIBLE);
    }

    @Override public void draw(Canvas canvas) {}

    /** the size within the spec: the minimum when unspecified, at most the spec's, or exactly it */
    private static int size(int size, int spec) {
        int mode = MeasureSpec.getMode(spec), specSize = MeasureSpec.getSize(spec);
        return mode == MeasureSpec.AT_MOST ? Math.min(size, specSize) : mode == MeasureSpec.EXACTLY ? specSize : size;
    }
    @Override protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        setMeasuredDimension(size(getSuggestedMinimumWidth(), widthMeasureSpec),
                             size(getSuggestedMinimumHeight(), heightMeasureSpec));
    }
}
