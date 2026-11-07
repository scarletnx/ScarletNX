// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2014 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.graphics;

/** A view's shape for its shadow and its clip: a rectangle with a radius, an oval or a path. */
public final class Outline {
    public static final int MODE_EMPTY = 0, MODE_ROUND_RECT = 1, MODE_PATH = 2;
    public int mMode = MODE_EMPTY;
    public final Rect mRect = new Rect();
    public float mRadius = Float.NEGATIVE_INFINITY;
    public Path mPath;
    public float mAlpha;

    public Outline() {}
    public Outline(Outline src) { set(src); }

    public void setEmpty() { mMode = MODE_EMPTY; mRect.setEmpty(); mRadius = Float.NEGATIVE_INFINITY; mPath = null; }
    public boolean isEmpty() { return mMode == MODE_EMPTY; }
    public boolean canClip() { return true; }
    public void setAlpha(float alpha) { mAlpha = alpha; }
    public float getAlpha() { return mAlpha; }
    public void set(Outline src) {
        mMode = src.mMode;
        mRect.set(src.mRect);
        mRadius = src.mRadius;
        mPath = src.mPath != null ? new Path(src.mPath) : null;
        mAlpha = src.mAlpha;
    }
    public void setRect(int left, int top, int right, int bottom) { setRoundRect(left, top, right, bottom, 0); }
    public void setRect(Rect r) { setRect(r.left, r.top, r.right, r.bottom); }
    public void setRoundRect(int left, int top, int right, int bottom, float radius) {
        if (left >= right || top >= bottom) { setEmpty(); return; }
        mMode = MODE_ROUND_RECT;
        mRect.set(left, top, right, bottom);
        mRadius = radius;
        mPath = null;
    }
    public void setRoundRect(Rect r, float radius) { setRoundRect(r.left, r.top, r.right, r.bottom, radius); }
    public boolean getRect(Rect outRect) {
        if (mMode != MODE_ROUND_RECT) return false;
        outRect.set(mRect);
        return true;
    }
    public float getRadius() { return mRadius; }
    public void setOval(int left, int top, int right, int bottom) {
        if (left >= right || top >= bottom) { setEmpty(); return; }
        if (bottom - top == right - left) { setRoundRect(left, top, right, bottom, (bottom - top) / 2f); return; }
        Path p = new Path();
        p.addOval(left, top, right, bottom, Path.Direction.CW);
        setPath(p);
    }
    public void setOval(Rect r) { setOval(r.left, r.top, r.right, r.bottom); }
    public void setPath(Path path) {
        if (path.isEmpty()) { setEmpty(); return; }
        mMode = MODE_PATH;
        mPath = new Path(path);
        mRect.setEmpty();
        mRadius = Float.NEGATIVE_INFINITY;
    }
    @Deprecated public void setConvexPath(Path convexPath) { setPath(convexPath); }
    public void offset(int dx, int dy) {
        if (mMode == MODE_ROUND_RECT) mRect.offset(dx, dy);
        else if (mMode == MODE_PATH) mPath.offset(dx, dy);
    }
}
