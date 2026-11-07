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

/** A drawable shrunk inside its bounds by its level (0 to 10000), toward its gravity. */
public class ScaleDrawable extends DrawableWrapper {
    private static final int MAX_LEVEL = 10000;
    private final int gravity;
    private final float scaleWidth, scaleHeight;
    private final Rect tmp = new Rect();

    public ScaleDrawable(Drawable drawable, int gravity, float scaleWidth, float scaleHeight) {
        super(drawable);
        this.gravity = gravity;
        this.scaleWidth = scaleWidth;
        this.scaleHeight = scaleHeight;
    }
    @Override public Drawable getDrawable() { return super.getDrawable(); }
    @Override public void draw(Canvas canvas) { if (getDrawable() != null && getLevel() != 0) getDrawable().draw(canvas); }
    @Override public int getOpacity() {
        Drawable dr = getDrawable();
        if (dr == null || getLevel() == 0) return PixelFormat.TRANSPARENT;
        return dr.getOpacity() == PixelFormat.OPAQUE && getLevel() < MAX_LEVEL ? PixelFormat.TRANSLUCENT : dr.getOpacity();
    }
    @Override protected boolean onLevelChange(int level) {
        super.onLevelChange(level);
        onBoundsChange(getBounds());
        invalidateSelf();
        return true;
    }
    @Override protected void onBoundsChange(Rect bounds) {
        int level = getLevel(), w = bounds.width(), h = bounds.height();
        if (scaleWidth > 0) w -= (int) (w * (MAX_LEVEL - level) * scaleWidth / MAX_LEVEL);
        if (scaleHeight > 0) h -= (int) (h * (MAX_LEVEL - level) * scaleHeight / MAX_LEVEL);
        Gravity.apply(gravity, w, h, bounds, tmp, getLayoutDirection());
        super.onBoundsChange(tmp);
    }
}
