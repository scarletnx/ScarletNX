// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

import java.util.List;

/** Lengths along a path's contours, and the positions and tangents at a distance, over the path flattened. */
public class PathMeasure {
    private List<float[]> contours = java.util.Collections.emptyList();
    private boolean[] closed = new boolean[0];
    private int index;
    private boolean forceClosed;
    private float[] lengths = new float[0];

    public PathMeasure() {}
    public PathMeasure(Path path, boolean forceClosed) { setPath(path, forceClosed); }

    public void setPath(Path path, boolean forceClosed) {
        this.forceClosed = forceClosed;
        index = 0;
        if (path == null) { contours = java.util.Collections.emptyList(); return; }
        closed = new boolean[path.verbCount() + 1];
        contours = path.flatten(0.25f, closed);
        lengths = new float[contours.size()];
        for (int i = 0; i < lengths.length; ++i) lengths[i] = length(i);
    }

    private boolean isClosed(int i) { return forceClosed || closed[i]; }

    private float length(int i) {
        float[] c = contours.get(i);
        float len = 0;
        for (int k = 2; k < c.length; k += 2) len += (float) Math.hypot(c[k] - c[k - 2], c[k + 1] - c[k - 1]);
        if (isClosed(i) && c.length >= 4) len += (float) Math.hypot(c[0] - c[c.length - 2], c[1] - c[c.length - 1]);
        return len;
    }

    public float getLength() { return index < lengths.length ? lengths[index] : 0; }
    public boolean isClosed() { return index < contours.size() && isClosed(index); }
    public boolean nextContour() { return ++index < contours.size(); }

    /** The polyline's points with the closing point appended for a closed contour. */
    private float[] points() {
        float[] c = contours.get(index);
        if (!isClosed(index) || c.length < 4) return c;
        float[] p = java.util.Arrays.copyOf(c, c.length + 2);
        p[c.length] = c[0];
        p[c.length + 1] = c[1];
        return p;
    }

    public boolean getPosTan(float distance, float[] pos, float[] tan) {
        if (index >= contours.size()) return false;
        float[] p = points();
        if (p.length < 4) {
            if (pos != null && p.length >= 2) { pos[0] = p[0]; pos[1] = p[1]; }
            return p.length >= 2;
        }
        distance = Math.max(0, Math.min(distance, getLength()));
        for (int k = 2; k < p.length; k += 2) {
            float dx = p[k] - p[k - 2], dy = p[k + 1] - p[k - 1], seg = (float) Math.hypot(dx, dy);
            if (distance <= seg || k == p.length - 2) {
                float t = seg == 0 ? 0 : Math.min(1, distance / seg);
                if (pos != null) { pos[0] = p[k - 2] + dx * t; pos[1] = p[k - 1] + dy * t; }
                if (tan != null) { tan[0] = seg == 0 ? 1 : dx / seg; tan[1] = seg == 0 ? 0 : dy / seg; }
                return true;
            }
            distance -= seg;
        }
        return false;
    }

    public boolean getMatrix(float distance, Matrix matrix, int flags) {
        float[] pos = new float[2], tan = new float[2];
        if (!getPosTan(distance, pos, tan)) return false;
        matrix.reset();
        if ((flags & 2) != 0) matrix.setSinCos(tan[1], tan[0], 0, 0);
        if ((flags & 1) != 0) matrix.postTranslate(pos[0], pos[1]);
        return true;
    }

    public boolean getSegment(float startD, float stopD, Path dst, boolean startWithMoveTo) {
        if (index >= contours.size()) return false;
        float len = getLength();
        startD = Math.max(0, startD);
        stopD = Math.min(len, stopD);
        if (startD >= stopD) return false;
        float[] p = points();
        float[] pos = new float[2];
        getPosTan(startD, pos, null);
        if (startWithMoveTo) dst.moveTo(pos[0], pos[1]); else dst.lineTo(pos[0], pos[1]);
        float d = 0;
        for (int k = 2; k < p.length; k += 2) {
            d += (float) Math.hypot(p[k] - p[k - 2], p[k + 1] - p[k - 1]);
            if (d > startD && d < stopD) dst.lineTo(p[k], p[k + 1]);
            if (d >= stopD) break;
        }
        getPosTan(stopD, pos, null);
        dst.lineTo(pos[0], pos[1]);
        return true;
    }

    public static final int POSITION_MATRIX_FLAG = 1, TANGENT_MATRIX_FLAG = 2;
}
