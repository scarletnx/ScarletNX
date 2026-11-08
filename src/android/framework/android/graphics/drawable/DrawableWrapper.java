// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2015 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.graphics.drawable;

import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Insets;
import android.graphics.Outline;
import android.graphics.PixelFormat;
import android.graphics.Rect;

/** A drawable that draws another one, and passes its state, level and bounds on to it. */
public abstract class DrawableWrapper extends Drawable implements Drawable.Callback {
    private Drawable drawable;

    public DrawableWrapper(Drawable dr) { setDrawable(dr); }

    public void setDrawable(Drawable dr) {
        if (drawable != null) drawable.setCallback(null);
        drawable = dr;
        if (dr != null) {
            dr.setCallback(this);
            dr.setVisible(isVisible(), true);
            dr.setState(getState());
            dr.setLevel(getLevel());
            dr.setBounds(getBounds());
            dr.setLayoutDirection(getLayoutDirection());
        }
        invalidateSelf();
    }
    public Drawable getDrawable() { return drawable; }

    @Override public void draw(Canvas canvas) { if (drawable != null) drawable.draw(canvas); }
    @Override public void setAlpha(int alpha) { if (drawable != null) drawable.setAlpha(alpha); }
    @Override public int getAlpha() { return drawable != null ? drawable.getAlpha() : 255; }
    @Override public void setColorFilter(ColorFilter colorFilter) { if (drawable != null) drawable.setColorFilter(colorFilter); }
    @Override public ColorFilter getColorFilter() { return drawable != null ? drawable.getColorFilter() : null; }
    @Override public void setTintList(android.content.res.ColorStateList tint) { if (drawable != null) drawable.setTintList(tint); }
    @Override public void setTintMode(android.graphics.PorterDuff.Mode mode) { if (drawable != null) drawable.setTintMode(mode); }
    @Override public int getOpacity() { return drawable != null ? drawable.getOpacity() : PixelFormat.TRANSPARENT; }
    @Override public boolean isStateful() { return drawable != null && drawable.isStateful(); }
    @Override public void jumpToCurrentState() { if (drawable != null) drawable.jumpToCurrentState(); }
    @Override protected boolean onStateChange(int[] state) {
        if (drawable == null || !drawable.isStateful()) return false;
        boolean changed = drawable.setState(state);
        if (changed) onBoundsChange(getBounds());
        return changed;
    }
    @Override protected boolean onLevelChange(int level) { return drawable != null && drawable.setLevel(level); }
    @Override protected void onBoundsChange(Rect bounds) { if (drawable != null) drawable.setBounds(bounds); }
    @Override public boolean onLayoutDirectionChanged(int layoutDirection) { return drawable != null && drawable.setLayoutDirection(layoutDirection); }
    @Override public boolean setVisible(boolean visible, boolean restart) {
        boolean changed = super.setVisible(visible, restart);
        return (drawable != null && drawable.setVisible(visible, restart)) || changed;
    }
    @Override public int getIntrinsicWidth() { return drawable != null ? drawable.getIntrinsicWidth() : -1; }
    @Override public int getIntrinsicHeight() { return drawable != null ? drawable.getIntrinsicHeight() : -1; }
    @Override public boolean getPadding(Rect padding) { return drawable != null ? drawable.getPadding(padding) : super.getPadding(padding); }
    @Override public Insets getOpticalInsets() { return drawable != null ? drawable.getOpticalInsets() : Insets.NONE; }
    @Override public void getOutline(Outline outline) { if (drawable != null) drawable.getOutline(outline); else super.getOutline(outline); }
    @Override public void setHotspot(float x, float y) { if (drawable != null) drawable.setHotspot(x, y); }
    @Override public void setHotspotBounds(int left, int top, int right, int bottom) { if (drawable != null) drawable.setHotspotBounds(left, top, right, bottom); }
    @Override public Drawable mutate() { if (drawable != null) drawable.mutate(); return this; }

    @Override public void invalidateDrawable(Drawable who) { invalidateSelf(); }
    @Override public void scheduleDrawable(Drawable who, Runnable what, long when) { scheduleSelf(what, when); }
    @Override public void unscheduleDrawable(Drawable who, Runnable what) { unscheduleSelf(what); }
}
