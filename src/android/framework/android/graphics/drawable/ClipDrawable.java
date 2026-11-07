// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.graphics.drawable;

import android.graphics.Canvas;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.view.Gravity;

/** A drawable clipped to a part of its bounds, by its level (0 to 10000). */
public class ClipDrawable extends DrawableWrapper {
    public static final int HORIZONTAL = 1, VERTICAL = 2;
    private static final int MAX_LEVEL = 10000;
    private final int gravity, orientation;
    private final Rect tmp = new Rect();

    public ClipDrawable(Drawable drawable, int gravity, int orientation) {
        super(drawable);
        this.gravity = gravity;
        this.orientation = orientation;
    }
    @Override protected boolean onLevelChange(int level) {
        super.onLevelChange(level);
        invalidateSelf();
        return true;
    }
    @Override public int getOpacity() {
        Drawable dr = getDrawable();
        if (dr == null || dr.getOpacity() == PixelFormat.TRANSPARENT || getLevel() == 0) return PixelFormat.TRANSPARENT;
        return getLevel() >= MAX_LEVEL ? dr.getOpacity() : PixelFormat.TRANSLUCENT;
    }
    @Override public void draw(Canvas canvas) {
        Drawable dr = getDrawable();
        int level = getLevel();
        if (dr == null || level == 0) return;
        Rect bounds = getBounds();
        int w = bounds.width(), h = bounds.height();
        if ((orientation & HORIZONTAL) != 0) w = (int) ((long) w * level / MAX_LEVEL);
        if ((orientation & VERTICAL) != 0) h = (int) ((long) h * level / MAX_LEVEL);
        Gravity.apply(gravity, w, h, bounds, tmp, getLayoutDirection());
        if (w > 0 && h > 0) {
            canvas.save();
            canvas.clipRect(tmp);
            dr.draw(canvas);
            canvas.restore();
        }
    }
}
