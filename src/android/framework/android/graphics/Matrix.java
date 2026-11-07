// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.graphics;

/**
 * A 3x3 matrix, row-major as Android's: MSCALE_X MSKEW_X MTRANS_X / MSKEW_Y MSCALE_Y MTRANS_Y / MPERSP_0 MPERSP_1
 * MPERSP_2. Pre operations apply before (on the right), post operations after (on the left).
 */
public class Matrix {
    public static final int MSCALE_X = 0, MSKEW_X = 1, MTRANS_X = 2, MSKEW_Y = 3, MSCALE_Y = 4, MTRANS_Y = 5,
        MPERSP_0 = 6, MPERSP_1 = 7, MPERSP_2 = 8;
    public static final Matrix IDENTITY_MATRIX = new Matrix() {
        @Override void changed() { throw new IllegalStateException("Matrix can not be modified"); }
    };

    public enum ScaleToFit { FILL, START, CENTER, END }

    final float[] m = {1, 0, 0, 0, 1, 0, 0, 0, 1};

    public Matrix() {}
    public Matrix(Matrix src) { if (src != null) System.arraycopy(src.m, 0, m, 0, 9); }

    void changed() {}

    public boolean isIdentity() { return m[0] == 1 && m[1] == 0 && m[2] == 0 && m[3] == 0 && m[4] == 1 && m[5] == 0 && m[6] == 0 && m[7] == 0 && m[8] == 1; }
    public boolean isAffine() { return m[6] == 0 && m[7] == 0 && m[8] == 1; }
    public boolean rectStaysRect() {
        return isAffine() && ((m[1] == 0 && m[3] == 0 && m[0] != 0 && m[4] != 0) || (m[0] == 0 && m[4] == 0 && m[1] != 0 && m[3] != 0));
    }
    public void set(Matrix src) { changed(); if (src == null) reset(); else System.arraycopy(src.m, 0, m, 0, 9); }
    public void reset() { changed(); setValues0(1, 0, 0, 0, 1, 0, 0, 0, 1); }
    private void setValues0(float a, float b, float c, float d, float e, float f, float g, float h, float i) {
        m[0] = a; m[1] = b; m[2] = c; m[3] = d; m[4] = e; m[5] = f; m[6] = g; m[7] = h; m[8] = i;
    }
    public void setTranslate(float dx, float dy) { changed(); setValues0(1, 0, dx, 0, 1, dy, 0, 0, 1); }
    public void setScale(float sx, float sy) { changed(); setValues0(sx, 0, 0, 0, sy, 0, 0, 0, 1); }
    public void setScale(float sx, float sy, float px, float py) { changed(); setValues0(sx, 0, px - sx * px, 0, sy, py - sy * py, 0, 0, 1); }
    public void setRotate(float degrees) { setRotate(degrees, 0, 0); }
    public void setRotate(float degrees, float px, float py) {
        double r = Math.toRadians(degrees);
        float s = (float) Math.sin(r), c = (float) Math.cos(r);
        if (Math.abs(degrees % 90) < 1e-6f) { s = Math.round(s); c = Math.round(c); }
        setSinCos(s, c, px, py);
    }
    public void setSinCos(float sin, float cos, float px, float py) {
        changed();
        setValues0(cos, -sin, sin * py + (1 - cos) * px, sin, cos, -sin * px + (1 - cos) * py, 0, 0, 1);
    }
    public void setSinCos(float sin, float cos) { setSinCos(sin, cos, 0, 0); }
    public void setSkew(float kx, float ky, float px, float py) { changed(); setValues0(1, kx, -kx * py, ky, 1, -ky * px, 0, 0, 1); }
    public void setSkew(float kx, float ky) { setSkew(kx, ky, 0, 0); }

    /** out = a * b */
    static void multiply(float[] out, float[] a, float[] b) {
        float[] r = new float[9];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
        System.arraycopy(r, 0, out, 0, 9);
    }

    public boolean setConcat(Matrix a, Matrix b) { changed(); multiply(m, a.m, b.m); return true; }
    private boolean pre(Matrix o) { changed(); multiply(m, m, o.m); return true; }
    private boolean post(Matrix o) { changed(); multiply(m, o.m, m); return true; }
    private static Matrix make() { return new Matrix(); }
    public boolean preTranslate(float dx, float dy) { Matrix t = make(); t.setTranslate(dx, dy); return pre(t); }
    public boolean preScale(float sx, float sy, float px, float py) { Matrix t = make(); t.setScale(sx, sy, px, py); return pre(t); }
    public boolean preScale(float sx, float sy) { Matrix t = make(); t.setScale(sx, sy); return pre(t); }
    public boolean preRotate(float degrees, float px, float py) { Matrix t = make(); t.setRotate(degrees, px, py); return pre(t); }
    public boolean preRotate(float degrees) { Matrix t = make(); t.setRotate(degrees); return pre(t); }
    public boolean preSkew(float kx, float ky, float px, float py) { Matrix t = make(); t.setSkew(kx, ky, px, py); return pre(t); }
    public boolean preSkew(float kx, float ky) { Matrix t = make(); t.setSkew(kx, ky); return pre(t); }
    public boolean preConcat(Matrix other) { return pre(other); }
    public boolean postTranslate(float dx, float dy) { Matrix t = make(); t.setTranslate(dx, dy); return post(t); }
    public boolean postScale(float sx, float sy, float px, float py) { Matrix t = make(); t.setScale(sx, sy, px, py); return post(t); }
    public boolean postScale(float sx, float sy) { Matrix t = make(); t.setScale(sx, sy); return post(t); }
    public boolean postRotate(float degrees, float px, float py) { Matrix t = make(); t.setRotate(degrees, px, py); return post(t); }
    public boolean postRotate(float degrees) { Matrix t = make(); t.setRotate(degrees); return post(t); }
    public boolean postSkew(float kx, float ky, float px, float py) { Matrix t = make(); t.setSkew(kx, ky, px, py); return post(t); }
    public boolean postSkew(float kx, float ky) { Matrix t = make(); t.setSkew(kx, ky); return post(t); }
    public boolean postConcat(Matrix other) { return post(other); }

    public boolean setRectToRect(RectF src, RectF dst, ScaleToFit stf) {
        changed();
        if (src.isEmpty()) { reset(); return false; }
        float sx = dst.width() / src.width(), sy = dst.height() / src.height(), tx, ty;
        if (stf != ScaleToFit.FILL) {
            float s = Math.min(sx, sy);
            sx = sy = s;
        }
        tx = dst.left - src.left * sx;
        ty = dst.top - src.top * sy;
        if (stf == ScaleToFit.CENTER || stf == ScaleToFit.END) {
            float dx = dst.width() - src.width() * sx, dy = dst.height() - src.height() * sy;
            if (stf == ScaleToFit.CENTER) { dx /= 2; dy /= 2; }
            tx += dx;
            ty += dy;
        }
        setValues0(sx, 0, tx, 0, sy, ty, 0, 0, 1);
        return true;
    }

    public boolean setPolyToPoly(float[] src, int srcIndex, float[] dst, int dstIndex, int pointCount) {
        if (pointCount == 0) { reset(); return true; }
        if (pointCount == 1) { setTranslate(dst[dstIndex] - src[srcIndex], dst[dstIndex + 1] - src[srcIndex + 1]); return true; }
        throw new UnsupportedOperationException("snx: Matrix.setPolyToPoly with " + pointCount + " points");
    }

    public boolean invert(Matrix inverse) {
        float[] a = m;
        double det = a[0] * ((double) a[4] * a[8] - (double) a[5] * a[7]) - a[1] * ((double) a[3] * a[8] - (double) a[5] * a[6])
            + a[2] * ((double) a[3] * a[7] - (double) a[4] * a[6]);
        if (det == 0 || Double.isNaN(det)) return false;
        double d = 1 / det;
        float[] r = {
            (float) ((a[4] * a[8] - a[5] * a[7]) * d), (float) ((a[2] * a[7] - a[1] * a[8]) * d), (float) ((a[1] * a[5] - a[2] * a[4]) * d),
            (float) ((a[5] * a[6] - a[3] * a[8]) * d), (float) ((a[0] * a[8] - a[2] * a[6]) * d), (float) ((a[2] * a[3] - a[0] * a[5]) * d),
            (float) ((a[3] * a[7] - a[4] * a[6]) * d), (float) ((a[1] * a[6] - a[0] * a[7]) * d), (float) ((a[0] * a[4] - a[1] * a[3]) * d)};
        if (inverse != null) { inverse.changed(); System.arraycopy(r, 0, inverse.m, 0, 9); }
        return true;
    }

    public void mapPoints(float[] dst, int dstIndex, float[] src, int srcIndex, int pointCount) {
        for (int i = 0; i < pointCount; ++i) {
            float x = src[srcIndex + 2 * i], y = src[srcIndex + 2 * i + 1];
            float w = m[6] * x + m[7] * y + m[8];
            float nx = m[0] * x + m[1] * y + m[2], ny = m[3] * x + m[4] * y + m[5];
            if (w != 1 && w != 0) { nx /= w; ny /= w; }
            dst[dstIndex + 2 * i] = nx;
            dst[dstIndex + 2 * i + 1] = ny;
        }
    }
    public void mapPoints(float[] dst, float[] src) { mapPoints(dst, 0, src, 0, src.length >> 1); }
    public void mapPoints(float[] pts) { mapPoints(pts, 0, pts, 0, pts.length >> 1); }
    public void mapVectors(float[] dst, int dstIndex, float[] src, int srcIndex, int vectorCount) {
        for (int i = 0; i < vectorCount; ++i) {
            float x = src[srcIndex + 2 * i], y = src[srcIndex + 2 * i + 1];
            dst[dstIndex + 2 * i] = m[0] * x + m[1] * y;
            dst[dstIndex + 2 * i + 1] = m[3] * x + m[4] * y;
        }
    }
    public void mapVectors(float[] vecs) { mapVectors(vecs, 0, vecs, 0, vecs.length >> 1); }
    public void mapVectors(float[] dst, float[] src) { mapVectors(dst, 0, src, 0, src.length >> 1); }
    public boolean mapRect(RectF dst, RectF src) {
        float[] p = {src.left, src.top, src.right, src.top, src.right, src.bottom, src.left, src.bottom};
        mapPoints(p);
        dst.set(Math.min(Math.min(p[0], p[2]), Math.min(p[4], p[6])), Math.min(Math.min(p[1], p[3]), Math.min(p[5], p[7])),
            Math.max(Math.max(p[0], p[2]), Math.max(p[4], p[6])), Math.max(Math.max(p[1], p[3]), Math.max(p[5], p[7])));
        return rectStaysRect();
    }
    public boolean mapRect(RectF rect) { return mapRect(rect, rect); }
    public float mapRadius(float radius) {
        float[] v = {radius, 0, 0, radius};
        mapVectors(v);
        return (float) Math.sqrt(Math.hypot(v[0], v[1]) * Math.hypot(v[2], v[3]));
    }

    public void getValues(float[] values) { System.arraycopy(m, 0, values, 0, 9); }
    public void setValues(float[] values) { changed(); System.arraycopy(values, 0, m, 0, 9); }

    /** The values, for the display list. */
    public float[] values() { return m; }

    @Override public boolean equals(Object o) { return o instanceof Matrix x && java.util.Arrays.equals(m, x.m); }
    @Override public int hashCode() { return java.util.Arrays.hashCode(m); }
    @Override public String toString() { return "Matrix" + java.util.Arrays.toString(m); }
    public String toShortString() { return java.util.Arrays.toString(m); }
}
