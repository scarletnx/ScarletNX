// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

import java.util.Arrays;

/**
 * A geometric path: verbs (move, line, quad, cubic, close) and their points. Arcs, ovals and round rectangles are
 * made of cubic Beziers of at most 90 degrees each. The display list carries the verbs and points; the renderer
 * flattens the curves for the transform they are drawn with.
 */
public class Path {
    public static final byte MOVE = 0, LINE = 1, QUAD = 2, CUBIC = 3, CLOSE = 4;
    private static final float KAPPA = 0.5522847498f;

    public enum Direction { CW, CCW }
    public enum FillType { WINDING, EVEN_ODD, INVERSE_WINDING, INVERSE_EVEN_ODD }
    public enum Op { DIFFERENCE, INTERSECT, UNION, XOR, REVERSE_DIFFERENCE }

    byte[] verbs = new byte[16];
    float[] points = new float[32];
    int verbCount, pointCount; // pointCount counts floats
    private FillType fill = FillType.WINDING;
    private float lastX, lastY, startX, startY;
    private boolean open; // a contour is begun (a move was made)
    private int generation;

    public Path() {}
    public Path(Path src) { if (src != null) set(src); }

    /** A count that changes with each change, so that a cache of a path's flattened form knows it is stale. */
    public int generation() { return generation; }
    public int verbCount() { return verbCount; }
    public byte[] verbs() { return verbs; }
    public float[] points() { return points; }
    public int pointCount() { return pointCount; }

    public void reset() { verbCount = pointCount = 0; fill = FillType.WINDING; open = false; lastX = lastY = startX = startY = 0; ++generation; }
    public void rewind() { reset(); }
    public void set(Path src) {
        if (src == this) return;
        verbs = Arrays.copyOf(src.verbs, Math.max(src.verbCount, 16));
        points = Arrays.copyOf(src.points, Math.max(src.pointCount, 32));
        verbCount = src.verbCount; pointCount = src.pointCount; fill = src.fill;
        lastX = src.lastX; lastY = src.lastY; startX = src.startX; startY = src.startY; open = src.open;
        ++generation;
    }
    public boolean isEmpty() { return verbCount == 0; }
    public boolean isInverseFillType() { return fill == FillType.INVERSE_WINDING || fill == FillType.INVERSE_EVEN_ODD; }
    public void toggleInverseFillType() { fill = FillType.values()[fill.ordinal() ^ 2]; ++generation; }
    public FillType getFillType() { return fill; }
    public void setFillType(FillType ft) { fill = ft; ++generation; }
    public boolean isRect(RectF rect) { return false; }
    public void incReserve(int extraPtCount) { ensure(1, 2 * extraPtCount); }

    /** A rough convexity test: a single closed contour that turns one way. */
    public boolean isConvex() {
        int moves = 0;
        for (int i = 0; i < verbCount; ++i) if (verbs[i] == MOVE) ++moves;
        if (moves > 1) return false;
        float cross = 0;
        int n = pointCount / 2;
        for (int i = 0; i < n; ++i) {
            float ax = points[2 * ((i + 1) % n)] - points[2 * i], ay = points[2 * ((i + 1) % n) + 1] - points[2 * i + 1];
            float bx = points[2 * ((i + 2) % n)] - points[2 * ((i + 1) % n)], by = points[2 * ((i + 2) % n) + 1] - points[2 * ((i + 1) % n) + 1];
            float c = ax * by - ay * bx;
            if (c != 0) {
                if (cross != 0 && Math.signum(c) != Math.signum(cross)) return false;
                cross = c;
            }
        }
        return true;
    }

    private void ensure(int moreVerbs, int moreFloats) {
        if (verbCount + moreVerbs > verbs.length) verbs = Arrays.copyOf(verbs, Math.max(verbs.length * 2, verbCount + moreVerbs));
        if (pointCount + moreFloats > points.length) points = Arrays.copyOf(points, Math.max(points.length * 2, pointCount + moreFloats));
    }
    private void verb(byte v) { verbs[verbCount++] = v; ++generation; }
    private void point(float x, float y) { points[pointCount++] = x; points[pointCount++] = y; lastX = x; lastY = y; }
    private void begin() { if (!open) moveTo(lastX, lastY); }

    public void moveTo(float x, float y) {
        ensure(1, 2);
        if (verbCount > 0 && verbs[verbCount - 1] == MOVE) { // a move after a move replaces it
            points[pointCount - 2] = x;
            points[pointCount - 1] = y;
            lastX = x; lastY = y;
            ++generation;
        } else {
            verb(MOVE);
            point(x, y);
        }
        startX = x; startY = y;
        open = true;
    }
    public void rMoveTo(float dx, float dy) { moveTo(lastX + dx, lastY + dy); }
    public void lineTo(float x, float y) { begin(); ensure(1, 2); verb(LINE); point(x, y); }
    public void rLineTo(float dx, float dy) { lineTo(lastX + dx, lastY + dy); }
    public void quadTo(float x1, float y1, float x2, float y2) {
        begin(); ensure(1, 4); verb(QUAD);
        points[pointCount++] = x1; points[pointCount++] = y1;
        point(x2, y2);
    }
    public void rQuadTo(float dx1, float dy1, float dx2, float dy2) { quadTo(lastX + dx1, lastY + dy1, lastX + dx2, lastY + dy2); }
    public void conicTo(float x1, float y1, float x2, float y2, float weight) {
        // a conic as a cubic of the same end tangents
        float k = 4f * weight / (3f * (1 + weight));
        cubicTo(lastX + k * (x1 - lastX), lastY + k * (y1 - lastY), x2 + k * (x1 - x2), y2 + k * (y1 - y2), x2, y2);
    }
    public void rConicTo(float dx1, float dy1, float dx2, float dy2, float weight) {
        conicTo(lastX + dx1, lastY + dy1, lastX + dx2, lastY + dy2, weight);
    }
    public void cubicTo(float x1, float y1, float x2, float y2, float x3, float y3) {
        begin(); ensure(1, 6); verb(CUBIC);
        points[pointCount++] = x1; points[pointCount++] = y1;
        points[pointCount++] = x2; points[pointCount++] = y2;
        point(x3, y3);
    }
    public void rCubicTo(float x1, float y1, float x2, float y2, float x3, float y3) {
        cubicTo(lastX + x1, lastY + y1, lastX + x2, lastY + y2, lastX + x3, lastY + y3);
    }
    public void close() {
        if (verbCount > 0 && verbs[verbCount - 1] != CLOSE) {
            ensure(1, 0);
            verb(CLOSE);
        }
        lastX = startX; lastY = startY;
        open = false;
    }

    /** An arc of the oval in rect, from startAngle through sweepAngle degrees, as cubics. */
    private void arc(float l, float t, float r, float b, float startAngle, float sweepAngle, boolean moveFirst) {
        float cx = (l + r) / 2, cy = (t + b) / 2, rx = (r - l) / 2, ry = (b - t) / 2;
        double a = Math.toRadians(startAngle);
        float sx = cx + rx * (float) Math.cos(a), sy = cy + ry * (float) Math.sin(a);
        if (moveFirst) moveTo(sx, sy); else lineTo(sx, sy);
        if (sweepAngle == 0) return;
        int segments = (int) Math.ceil(Math.abs(sweepAngle) / 90 - 1e-4);
        double step = Math.toRadians(sweepAngle / segments);
        double k = 4.0 / 3.0 * Math.tan(step / 4);
        for (int i = 0; i < segments; ++i) {
            double a0 = a + i * step, a1 = a0 + step;
            float c0 = (float) Math.cos(a0), s0 = (float) Math.sin(a0), c1 = (float) Math.cos(a1), s1 = (float) Math.sin(a1);
            cubicTo(cx + rx * (c0 - (float) k * s0), cy + ry * (s0 + (float) k * c0),
                cx + rx * (c1 + (float) k * s1), cy + ry * (s1 - (float) k * c1), cx + rx * c1, cy + ry * s1);
        }
    }
    public void arcTo(RectF oval, float startAngle, float sweepAngle, boolean forceMoveTo) {
        arcTo(oval.left, oval.top, oval.right, oval.bottom, startAngle, sweepAngle, forceMoveTo);
    }
    public void arcTo(RectF oval, float startAngle, float sweepAngle) { arcTo(oval, startAngle, sweepAngle, false); }
    public void arcTo(float left, float top, float right, float bottom, float startAngle, float sweepAngle, boolean forceMoveTo) {
        arc(left, top, right, bottom, startAngle, sweepAngle, forceMoveTo || !open);
    }
    public void addArc(RectF oval, float startAngle, float sweepAngle) { addArc(oval.left, oval.top, oval.right, oval.bottom, startAngle, sweepAngle); }
    public void addArc(float left, float top, float right, float bottom, float startAngle, float sweepAngle) {
        arc(left, top, right, bottom, startAngle, sweepAngle, true);
    }

    public void addRect(RectF r, Direction dir) { addRect(r.left, r.top, r.right, r.bottom, dir); }
    public void addRect(float left, float top, float right, float bottom, Direction dir) {
        moveTo(left, top);
        if (dir == Direction.CW) { lineTo(right, top); lineTo(right, bottom); lineTo(left, bottom); }
        else { lineTo(left, bottom); lineTo(right, bottom); lineTo(right, top); }
        close();
    }
    public void addOval(RectF oval, Direction dir) { addOval(oval.left, oval.top, oval.right, oval.bottom, dir); }
    public void addOval(float left, float top, float right, float bottom, Direction dir) {
        addRoundRect(left, top, right, bottom, (right - left) / 2, (bottom - top) / 2, dir);
    }
    public void addCircle(float x, float y, float radius, Direction dir) { addOval(x - radius, y - radius, x + radius, y + radius, dir); }
    public void addRoundRect(RectF rect, float rx, float ry, Direction dir) { addRoundRect(rect.left, rect.top, rect.right, rect.bottom, rx, ry, dir); }
    public void addRoundRect(float left, float top, float right, float bottom, float rx, float ry, Direction dir) {
        addRoundRect(left, top, right, bottom, new float[] {rx, ry, rx, ry, rx, ry, rx, ry}, dir);
    }
    public void addRoundRect(RectF rect, float[] radii, Direction dir) { addRoundRect(rect.left, rect.top, rect.right, rect.bottom, radii, dir); }

    /** A round rectangle, radii x and y of the top left, top right, bottom right and bottom left corners. */
    public void addRoundRect(float l, float t, float r, float b, float[] radii, Direction dir) {
        if (l >= r || t >= b) return;
        float w = r - l, h = b - t;
        float[] rad = new float[8];
        for (int i = 0; i < 8; ++i) rad[i] = Math.max(0, radii[i]);
        // radii too big for the sides are scaled down together, as Skia does
        float scale = 1;
        scale = Math.min(scale, fit(w, rad[0] + rad[2])); scale = Math.min(scale, fit(w, rad[4] + rad[6]));
        scale = Math.min(scale, fit(h, rad[1] + rad[7])); scale = Math.min(scale, fit(h, rad[3] + rad[5]));
        if (scale < 1) for (int i = 0; i < 8; ++i) rad[i] *= scale;
        float tlx = rad[0], tly = rad[1], trx = rad[2], try_ = rad[3], brx = rad[4], bry = rad[5], blx = rad[6], bly = rad[7];
        float k = KAPPA;
        if (dir == Direction.CW) {
            moveTo(l + tlx, t);
            lineTo(r - trx, t);
            if (trx > 0 || try_ > 0) cubicTo(r - trx + k * trx, t, r, t + try_ - k * try_, r, t + try_);
            lineTo(r, b - bry);
            if (brx > 0 || bry > 0) cubicTo(r, b - bry + k * bry, r - brx + k * brx, b, r - brx, b);
            lineTo(l + blx, b);
            if (blx > 0 || bly > 0) cubicTo(l + blx - k * blx, b, l, b - bly + k * bly, l, b - bly);
            lineTo(l, t + tly);
            if (tlx > 0 || tly > 0) cubicTo(l, t + tly - k * tly, l + tlx - k * tlx, t, l + tlx, t);
        } else {
            moveTo(l + tlx, t);
            if (tlx > 0 || tly > 0) cubicTo(l + tlx - k * tlx, t, l, t + tly - k * tly, l, t + tly);
            lineTo(l, b - bly);
            if (blx > 0 || bly > 0) cubicTo(l, b - bly + k * bly, l + blx - k * blx, b, l + blx, b);
            lineTo(r - brx, b);
            if (brx > 0 || bry > 0) cubicTo(r - brx + k * brx, b, r, b - bry + k * bry, r, b - bry);
            lineTo(r, t + try_);
            if (trx > 0 || try_ > 0) cubicTo(r, t + try_ - k * try_, r - trx + k * trx, t, r - trx, t);
        }
        close();
    }
    private static float fit(float side, float sum) { return sum > side && sum > 0 ? side / sum : 1; }

    public void addPath(Path src, float dx, float dy) {
        Path p = new Path(src);
        p.offset(dx, dy);
        append(p);
    }
    public void addPath(Path src) { append(src); }
    public void addPath(Path src, Matrix matrix) {
        Path p = new Path(src);
        p.transform(matrix);
        append(p);
    }
    private void append(Path src) {
        ensure(src.verbCount, src.pointCount);
        System.arraycopy(src.verbs, 0, verbs, verbCount, src.verbCount);
        System.arraycopy(src.points, 0, points, pointCount, src.pointCount);
        verbCount += src.verbCount;
        pointCount += src.pointCount;
        if (src.pointCount >= 2) { lastX = points[pointCount - 2]; lastY = points[pointCount - 1]; }
        open = src.open;
        startX = src.startX; startY = src.startY;
        ++generation;
    }

    public void offset(float dx, float dy, Path dst) {
        if (dst != null && dst != this) { dst.set(this); dst.offset(dx, dy); return; }
        offset(dx, dy);
    }
    public void offset(float dx, float dy) {
        for (int i = 0; i < pointCount; i += 2) { points[i] += dx; points[i + 1] += dy; }
        lastX += dx; lastY += dy; startX += dx; startY += dy;
        ++generation;
    }
    public void setLastPoint(float dx, float dy) {
        if (pointCount >= 2) { points[pointCount - 2] = dx; points[pointCount - 1] = dy; lastX = dx; lastY = dy; ++generation; }
        else moveTo(dx, dy);
    }
    public void transform(Matrix matrix, Path dst) {
        if (dst != null && dst != this) { dst.set(this); dst.transform(matrix); return; }
        transform(matrix);
    }
    public void transform(Matrix matrix) {
        matrix.mapPoints(points, 0, points, 0, pointCount / 2);
        float[] p = {lastX, lastY, startX, startY};
        matrix.mapPoints(p);
        lastX = p[0]; lastY = p[1]; startX = p[2]; startY = p[3];
        ++generation;
    }

    /** The bounds of the points (the control points too, as Android's computeBounds gives them). */
    public void computeBounds(RectF bounds, boolean exact) {
        if (pointCount == 0) { bounds.set(0, 0, 0, 0); return; }
        float l = points[0], t = points[1], r = l, b = t;
        for (int i = 2; i < pointCount; i += 2) {
            l = Math.min(l, points[i]); r = Math.max(r, points[i]);
            t = Math.min(t, points[i + 1]); b = Math.max(b, points[i + 1]);
        }
        bounds.set(l, t, r, b);
    }

    /**
     * Boolean operations: a union keeps both outlines with the winding rule, a difference adds the second reversed,
     * which is exact for the shapes that do not overlap themselves (rounded and cut corners); an intersection of
     * two rectangles is exact, other intersections keep the first path.
     */
    public boolean op(Path path, Op op) { return op(this, path, op); }
    public boolean op(Path path1, Path path2, Op op) {
        Path a = new Path(path1), b = new Path(path2);
        switch (op) {
            case UNION:
            case XOR:
                set(a); append(b); setFillType(op == Op.XOR ? FillType.EVEN_ODD : FillType.WINDING);
                return true;
            case DIFFERENCE:
                set(a); append(b); setFillType(FillType.EVEN_ODD);
                return true;
            case REVERSE_DIFFERENCE:
                set(b); append(a); setFillType(FillType.EVEN_ODD);
                return true;
            case INTERSECT: {
                RectF ra = new RectF(), rb = new RectF();
                a.computeBounds(ra, true); b.computeBounds(rb, true);
                set(a);
                if (a.verbCount <= 6 && b.verbCount <= 6) {
                    reset();
                    if (ra.intersect(rb)) addRect(ra, Direction.CW);
                }
                return true;
            }
            default:
                return false;
        }
    }

    public float[] approximate(float acceptableError) {
        PathMeasure m = new PathMeasure(this, false);
        java.util.ArrayList<float[]> pts = new java.util.ArrayList<>();
        float total = 0;
        do total += m.getLength(); while (m.nextContour());
        m.setPath(this, false);
        float done = 0;
        do {
            float len = m.getLength(), step = Math.max(acceptableError * 4, len / 64);
            float[] pos = new float[2];
            for (float d = 0; d <= len; d += step) {
                m.getPosTan(d, pos, null);
                pts.add(new float[] {total == 0 ? 0 : (done + d) / total, pos[0], pos[1]});
            }
            done += len;
        } while (m.nextContour());
        float[] out = new float[pts.size() * 3];
        for (int i = 0; i < pts.size(); ++i) System.arraycopy(pts.get(i), 0, out, i * 3, 3);
        return out;
    }

    /** The path as polylines, one for each contour: curves flattened to within tolerance. */
    public java.util.List<float[]> flatten(float tolerance, boolean[] closedOut) {
        java.util.ArrayList<float[]> contours = new java.util.ArrayList<>();
        float[] cur = new float[16];
        int n = 0, p = 0;
        int contour = -1;
        float x = 0, y = 0;
        java.util.ArrayList<Boolean> closed = new java.util.ArrayList<>();
        for (int i = 0; i < verbCount; ++i) {
            byte v = verbs[i];
            if (v == MOVE) {
                if (n > 0) { contours.add(Arrays.copyOf(cur, n)); closed.add(false); }
                n = 0;
                x = points[p++]; y = points[p++];
                if (n + 2 > cur.length) cur = Arrays.copyOf(cur, cur.length * 2);
                cur[n++] = x; cur[n++] = y;
            } else if (v == LINE) {
                x = points[p++]; y = points[p++];
                if (n + 2 > cur.length) cur = Arrays.copyOf(cur, cur.length * 2);
                cur[n++] = x; cur[n++] = y;
            } else if (v == QUAD || v == CUBIC) {
                int count = v == QUAD ? 2 : 3;
                float[] c = new float[2 + 2 * count];
                c[0] = x; c[1] = y;
                for (int k = 0; k < 2 * count; ++k) c[2 + k] = points[p++];
                float len = 0;
                for (int k = 0; k < count; ++k) len += (float) Math.hypot(c[2 * k + 2] - c[2 * k], c[2 * k + 3] - c[2 * k + 1]);
                int steps = Math.max(1, Math.min(256, (int) Math.ceil(Math.sqrt(len / Math.max(tolerance, 1e-3f)))));
                for (int s = 1; s <= steps; ++s) {
                    float t = s / (float) steps, u = 1 - t, px, py;
                    if (v == QUAD) {
                        px = u * u * c[0] + 2 * u * t * c[2] + t * t * c[4];
                        py = u * u * c[1] + 2 * u * t * c[3] + t * t * c[5];
                    } else {
                        px = u * u * u * c[0] + 3 * u * u * t * c[2] + 3 * u * t * t * c[4] + t * t * t * c[6];
                        py = u * u * u * c[1] + 3 * u * u * t * c[3] + 3 * u * t * t * c[5] + t * t * t * c[7];
                    }
                    if (n + 2 > cur.length) cur = Arrays.copyOf(cur, cur.length * 2);
                    cur[n++] = px; cur[n++] = py;
                }
                x = c[c.length - 2]; y = c[c.length - 1];
            } else if (v == CLOSE) {
                if (n > 0) { contours.add(Arrays.copyOf(cur, n)); closed.add(true); x = cur[0]; y = cur[1]; }
                n = 0;
            }
        }
        if (n > 0) { contours.add(Arrays.copyOf(cur, n)); closed.add(false); }
        if (closedOut != null) for (int i = 0; i < closed.size() && i < closedOut.length; ++i) closedOut[i] = closed.get(i);
        return contours;
    }

    @Override public String toString() { return "Path{" + verbCount + " verbs}"; }
}
