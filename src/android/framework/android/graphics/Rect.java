// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.graphics;

/** An integer rectangle. */
public final class Rect implements android.os.Parcelable {
    public int left, top, right, bottom;

    public Rect() {}
    public Rect(int left, int top, int right, int bottom) { set(left, top, right, bottom); }
    public Rect(Rect r) { if (r != null) set(r); }
    public Rect(RectF r) { set((int) r.left, (int) r.top, (int) r.right, (int) r.bottom); }

    public void set(int left, int top, int right, int bottom) { this.left = left; this.top = top; this.right = right; this.bottom = bottom; }
    public void set(Rect r) { set(r.left, r.top, r.right, r.bottom); }
    public void setEmpty() { left = top = right = bottom = 0; }
    public boolean isEmpty() { return left >= right || top >= bottom; }
    public int width() { return right - left; }
    public int height() { return bottom - top; }
    public int centerX() { return (left + right) >> 1; }
    public int centerY() { return (top + bottom) >> 1; }
    public float exactCenterX() { return (left + right) * 0.5f; }
    public float exactCenterY() { return (top + bottom) * 0.5f; }
    public void offset(int dx, int dy) { left += dx; top += dy; right += dx; bottom += dy; }
    public void offsetTo(int x, int y) { right += x - left; bottom += y - top; left = x; top = y; }
    public void inset(int dx, int dy) { left += dx; top += dy; right -= dx; bottom -= dy; }
    public void inset(int l, int t, int r, int b) { left += l; top += t; right -= r; bottom -= b; }
    public void inset(Insets i) { inset(i.left, i.top, i.right, i.bottom); }
    public boolean contains(int x, int y) { return left < right && top < bottom && x >= left && x < right && y >= top && y < bottom; }
    public boolean contains(int l, int t, int r, int b) { return left < right && top < bottom && left <= l && top <= t && right >= r && bottom >= b; }
    public boolean contains(Rect r) { return contains(r.left, r.top, r.right, r.bottom); }
    public boolean intersect(int l, int t, int r, int b) {
        if (left < r && l < right && top < b && t < bottom) {
            left = Math.max(left, l); top = Math.max(top, t); right = Math.min(right, r); bottom = Math.min(bottom, b);
            return true;
        }
        return false;
    }
    public boolean intersect(Rect r) { return intersect(r.left, r.top, r.right, r.bottom); }
    public boolean setIntersect(Rect a, Rect b) {
        if (a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom) {
            set(Math.max(a.left, b.left), Math.max(a.top, b.top), Math.min(a.right, b.right), Math.min(a.bottom, b.bottom));
            return true;
        }
        return false;
    }
    public boolean intersects(int l, int t, int r, int b) { return left < r && l < right && top < b && t < bottom; }
    public static boolean intersects(Rect a, Rect b) { return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom; }
    public void union(int l, int t, int r, int b) {
        if (l >= r || t >= b) return;
        if (left < right && top < bottom) {
            left = Math.min(left, l); top = Math.min(top, t); right = Math.max(right, r); bottom = Math.max(bottom, b);
        } else {
            set(l, t, r, b);
        }
    }
    public void union(Rect r) { union(r.left, r.top, r.right, r.bottom); }
    public void union(int x, int y) {
        if (x < left) left = x; else if (x > right) right = x;
        if (y < top) top = y; else if (y > bottom) bottom = y;
    }
    public void sort() {
        if (left > right) { int t = left; left = right; right = t; }
        if (top > bottom) { int t = top; top = bottom; bottom = t; }
    }
    public void scale(float s) {
        if (s != 1) { left = (int) (left * s + 0.5f); top = (int) (top * s + 0.5f); right = (int) (right * s + 0.5f); bottom = (int) (bottom * s + 0.5f); }
    }
    @Override public boolean equals(Object o) { return o instanceof Rect r && r.left == left && r.top == top && r.right == right && r.bottom == bottom; }
    @Override public int hashCode() { return ((left * 31 + top) * 31 + right) * 31 + bottom; }
    @Override public String toString() { return "Rect(" + left + ", " + top + " - " + right + ", " + bottom + ")"; }
    public String toShortString() { return "[" + left + "," + top + "][" + right + "," + bottom + "]"; }
    public String flattenToString() { return left + " " + top + " " + right + " " + bottom; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeInt(left); out.writeInt(top); out.writeInt(right); out.writeInt(bottom); }
    public static final android.os.Parcelable.Creator<Rect> CREATOR = new android.os.Parcelable.Creator<>() {
        @Override public Rect createFromParcel(android.os.Parcel in) { return new Rect(in.readInt(), in.readInt(), in.readInt(), in.readInt()); }
        @Override public Rect[] newArray(int size) { return new Rect[size]; }
    };
}
