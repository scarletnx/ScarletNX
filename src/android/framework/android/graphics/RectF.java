// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.graphics;

/** A float rectangle. */
public class RectF implements android.os.Parcelable {
    public float left, top, right, bottom;

    public RectF() {}
    public RectF(float left, float top, float right, float bottom) { set(left, top, right, bottom); }
    public RectF(RectF r) { if (r != null) set(r); }
    public RectF(Rect r) { if (r != null) set(r); }

    public final boolean isEmpty() { return !(left < right && top < bottom); }
    public final float width() { return right - left; }
    public final float height() { return bottom - top; }
    public final float centerX() { return (left + right) * 0.5f; }
    public final float centerY() { return (top + bottom) * 0.5f; }
    public void setEmpty() { left = top = right = bottom = 0; }
    public void set(float left, float top, float right, float bottom) { this.left = left; this.top = top; this.right = right; this.bottom = bottom; }
    public void set(RectF r) { set(r.left, r.top, r.right, r.bottom); }
    public void set(Rect r) { set(r.left, r.top, r.right, r.bottom); }
    public void offset(float dx, float dy) { left += dx; top += dy; right += dx; bottom += dy; }
    public void offsetTo(float x, float y) { right += x - left; bottom += y - top; left = x; top = y; }
    public void inset(float dx, float dy) { left += dx; top += dy; right -= dx; bottom -= dy; }
    public boolean contains(float x, float y) { return left < right && top < bottom && x >= left && x < right && y >= top && y < bottom; }
    public boolean contains(float l, float t, float r, float b) { return left < right && top < bottom && left <= l && top <= t && right >= r && bottom >= b; }
    public boolean contains(RectF r) { return contains(r.left, r.top, r.right, r.bottom); }
    public boolean intersect(float l, float t, float r, float b) {
        if (left < r && l < right && top < b && t < bottom) {
            left = Math.max(left, l); top = Math.max(top, t); right = Math.min(right, r); bottom = Math.min(bottom, b);
            return true;
        }
        return false;
    }
    public boolean intersect(RectF r) { return intersect(r.left, r.top, r.right, r.bottom); }
    public boolean setIntersect(RectF a, RectF b) {
        if (a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom) {
            set(Math.max(a.left, b.left), Math.max(a.top, b.top), Math.min(a.right, b.right), Math.min(a.bottom, b.bottom));
            return true;
        }
        return false;
    }
    public boolean intersects(float l, float t, float r, float b) { return left < r && l < right && top < b && t < bottom; }
    public static boolean intersects(RectF a, RectF b) { return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom; }
    public void round(Rect dst) { dst.set(Math.round(left), Math.round(top), Math.round(right), Math.round(bottom)); }
    public void roundOut(Rect dst) { dst.set((int) Math.floor(left), (int) Math.floor(top), (int) Math.ceil(right), (int) Math.ceil(bottom)); }
    public void union(float l, float t, float r, float b) {
        if (l >= r || t >= b) return;
        if (left < right && top < bottom) {
            left = Math.min(left, l); top = Math.min(top, t); right = Math.max(right, r); bottom = Math.max(bottom, b);
        } else {
            set(l, t, r, b);
        }
    }
    public void union(RectF r) { union(r.left, r.top, r.right, r.bottom); }
    public void union(float x, float y) {
        if (x < left) left = x; else if (x > right) right = x;
        if (y < top) top = y; else if (y > bottom) bottom = y;
    }
    public void sort() {
        if (left > right) { float t = left; left = right; right = t; }
        if (top > bottom) { float t = top; top = bottom; bottom = t; }
    }
    @Override public boolean equals(Object o) { return o instanceof RectF r && r.left == left && r.top == top && r.right == right && r.bottom == bottom; }
    @Override public int hashCode() { return Float.floatToIntBits(left) * 31 + Float.floatToIntBits(top) * 17 + Float.floatToIntBits(right) * 7 + Float.floatToIntBits(bottom); }
    @Override public String toString() { return "RectF(" + left + ", " + top + ", " + right + ", " + bottom + ")"; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeFloat(left); out.writeFloat(top); out.writeFloat(right); out.writeFloat(bottom); }
}
