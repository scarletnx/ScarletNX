// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.graphics;

import java.util.ArrayList;
import snx.android.hwui.DisplayList;
import snx.android.hwui.Text;

/**
 * Draws into a display list (docs/platforms/android-apps/hwui.md): a RenderNode's, a Picture's or a Bitmap's. The matrix and the
 * clip's bounds are kept here too, for the queries; the renderer draws the operations with Vulkan.
 */
public class Canvas {
    public static final int ALL_SAVE_FLAG = 0x1f, MATRIX_SAVE_FLAG = 0x01, CLIP_SAVE_FLAG = 0x02;

    public enum EdgeType { BW, AA }
    public enum VertexMode { TRIANGLES, TRIANGLE_STRIP, TRIANGLE_FAN }

    DisplayList list;
    DisplayList.Writer w;
    private Bitmap bitmap;
    int width, height;
    private int density;
    private DrawFilter drawFilter;
    /** The state stack: the matrix and the clip bounds (device coordinates) at each save. */
    private final ArrayList<float[]> states = new ArrayList<>();
    private final Matrix matrix = new Matrix();
    private final RectF clip = new RectF();

    public Canvas() {
        list = new DisplayList();
        w = list.writer();
        clip.set(-1e9f, -1e9f, 1e9f, 1e9f);
    }

    public Canvas(Bitmap bitmap) {
        this();
        setBitmap(bitmap);
    }

    /** A canvas recording into a list, width by height. */
    protected Canvas(DisplayList list, int width, int height) {
        this.list = list;
        w = list.writer();
        begin(width, height);
    }

    void begin(int width, int height) {
        this.width = width;
        this.height = height;
        states.clear();
        matrix.reset();
        clip.set(0, 0, width, height);
    }

    public void setBitmap(Bitmap bitmap) {
        this.bitmap = bitmap;
        if (bitmap != null) {
            list = bitmap.drawing();
            w = list.writer();
            begin(bitmap.getWidth(), bitmap.getHeight());
        }
    }

    public boolean isHardwareAccelerated() { return bitmap == null; }
    public boolean isOpaque() { return bitmap != null && !bitmap.hasAlpha(); }
    public int getWidth() { return width; }
    public int getHeight() { return height; }
    public int getDensity() { return density; }
    public void setDensity(int density) { this.density = density; }
    public int getMaximumBitmapWidth() { return 16384; }
    public int getMaximumBitmapHeight() { return 16384; }
    public void enableZ() {}
    public void disableZ() {}
    public DrawFilter getDrawFilter() { return drawFilter; }
    public void setDrawFilter(DrawFilter filter) { drawFilter = filter; }

    // ---- state ----

    private void push() {
        float[] s = new float[13];
        matrix.getValues(s);
        s[9] = clip.left; s[10] = clip.top; s[11] = clip.right; s[12] = clip.bottom;
        states.add(s);
    }

    public int save() {
        push();
        w.u8(DisplayList.SAVE);
        return states.size();
    }
    public int save(int saveFlags) { return save(); }

    public int saveLayer(float left, float top, float right, float bottom, Paint paint) {
        push();
        w.u8(DisplayList.SAVE_LAYER);
        w.f32(left); w.f32(top); w.f32(right); w.f32(bottom);
        w.i32(paint != null ? paint.getAlpha() : 255);
        w.u8(paint != null && paint.getBlendMode() != null ? paint.getBlendMode().skia() : BlendMode.SRC_OVER.skia());
        return states.size();
    }
    public int saveLayer(float l, float t, float r, float b, Paint paint, int saveFlags) { return saveLayer(l, t, r, b, paint); }
    public int saveLayer(RectF bounds, Paint paint) {
        return bounds != null ? saveLayer(bounds.left, bounds.top, bounds.right, bounds.bottom, paint) : saveLayer(0, 0, width, height, paint);
    }
    public int saveLayer(RectF bounds, Paint paint, int saveFlags) { return saveLayer(bounds, paint); }
    public int saveLayerAlpha(float l, float t, float r, float b, int alpha) {
        Paint p = new Paint();
        p.setAlpha(Math.min(255, Math.max(0, alpha)));
        return saveLayer(l, t, r, b, p);
    }
    public int saveLayerAlpha(float l, float t, float r, float b, int alpha, int saveFlags) { return saveLayerAlpha(l, t, r, b, alpha); }
    public int saveLayerAlpha(RectF bounds, int alpha) {
        return bounds != null ? saveLayerAlpha(bounds.left, bounds.top, bounds.right, bounds.bottom, alpha) : saveLayerAlpha(0, 0, width, height, alpha);
    }
    public int saveLayerAlpha(RectF bounds, int alpha, int saveFlags) { return saveLayerAlpha(bounds, alpha); }

    public void restore() {
        if (states.isEmpty()) throw new IllegalStateException("Underflow in restore - more restores than saves");
        float[] s = states.remove(states.size() - 1);
        matrix.setValues(java.util.Arrays.copyOf(s, 9));
        clip.set(s[9], s[10], s[11], s[12]);
        w.u8(DisplayList.RESTORE);
    }
    public int getSaveCount() { return states.size() + 1; }
    public void restoreToCount(int saveCount) {
        if (saveCount < 1) throw new IllegalArgumentException("Underflow in restoreToCount - more restores than saves");
        while (states.size() + 1 > saveCount) restore();
    }

    public void translate(float dx, float dy) {
        if (dx == 0 && dy == 0) return;
        matrix.preTranslate(dx, dy);
        w.u8(DisplayList.TRANSLATE); w.f32(dx); w.f32(dy);
    }
    public void scale(float sx, float sy) {
        if (sx == 1 && sy == 1) return;
        matrix.preScale(sx, sy);
        w.u8(DisplayList.SCALE); w.f32(sx); w.f32(sy);
    }
    public final void scale(float sx, float sy, float px, float py) {
        if (sx == 1 && sy == 1) return;
        translate(px, py);
        scale(sx, sy);
        translate(-px, -py);
    }
    public void rotate(float degrees) {
        if (degrees == 0) return;
        matrix.preRotate(degrees);
        w.u8(DisplayList.ROTATE); w.f32(degrees);
    }
    public final void rotate(float degrees, float px, float py) {
        if (degrees == 0) return;
        translate(px, py);
        rotate(degrees);
        translate(-px, -py);
    }
    public void skew(float sx, float sy) {
        Matrix m = new Matrix();
        m.setSkew(sx, sy);
        concat(m);
    }
    public void concat(Matrix m) {
        if (m == null || m.isIdentity()) return;
        matrix.preConcat(m);
        w.u8(DisplayList.CONCAT);
        for (float v : m.values()) w.f32(v);
    }
    public void setMatrix(Matrix m) {
        matrix.set(m);
        w.u8(DisplayList.SET_MATRIX);
        for (float v : (m != null ? m : new Matrix()).values()) w.f32(v);
    }
    @Deprecated public void getMatrix(Matrix ctm) { ctm.set(matrix); }
    @Deprecated public final Matrix getMatrix() { return new Matrix(matrix); }

    // ---- clips ----

    private boolean clipRect0(float l, float t, float r, float b, int op) {
        w.u8(DisplayList.CLIP_RECT); w.f32(l); w.f32(t); w.f32(r); w.f32(b); w.u8(op);
        if (op == 0) {
            RectF d = new RectF(l, t, r, b);
            matrix.mapRect(d);
            if (!clip.intersect(d)) clip.set(0, 0, 0, 0);
        }
        return !clip.isEmpty();
    }
    public boolean clipRect(float left, float top, float right, float bottom) { return clipRect0(left, top, right, bottom, 0); }
    public boolean clipRect(int left, int top, int right, int bottom) { return clipRect0(left, top, right, bottom, 0); }
    public boolean clipRect(Rect r) { return clipRect0(r.left, r.top, r.right, r.bottom, 0); }
    public boolean clipRect(RectF r) { return clipRect0(r.left, r.top, r.right, r.bottom, 0); }
    public boolean clipRect(float l, float t, float r, float b, Region.Op op) { return clipRect0(l, t, r, b, op == Region.Op.DIFFERENCE ? 1 : 0); }
    public boolean clipRect(Rect r, Region.Op op) { return clipRect(r.left, r.top, r.right, r.bottom, op); }
    public boolean clipRect(RectF r, Region.Op op) { return clipRect(r.left, r.top, r.right, r.bottom, op); }
    public boolean clipOutRect(float l, float t, float r, float b) { return clipRect0(l, t, r, b, 1); }
    public boolean clipOutRect(int l, int t, int r, int b) { return clipRect0(l, t, r, b, 1); }
    public boolean clipOutRect(Rect r) { return clipRect0(r.left, r.top, r.right, r.bottom, 1); }
    public boolean clipOutRect(RectF r) { return clipRect0(r.left, r.top, r.right, r.bottom, 1); }

    private boolean clipPath0(Path path, int op) {
        w.u8(DisplayList.CLIP_PATH);
        writePath(path);
        w.u8(op);
        if (op == 0) {
            RectF b = new RectF();
            path.computeBounds(b, true);
            matrix.mapRect(b);
            if (!path.isInverseFillType() && !clip.intersect(b)) clip.set(0, 0, 0, 0);
        }
        return !clip.isEmpty();
    }
    public boolean clipPath(Path path) { return clipPath0(path, 0); }
    public boolean clipPath(Path path, Region.Op op) { return clipPath0(path, op == Region.Op.DIFFERENCE ? 1 : 0); }
    public boolean clipOutPath(Path path) { return clipPath0(path, 1); }
    @Deprecated public boolean clipRegion(Region region, Region.Op op) { return clipRect(region.getBounds(), op); }
    @Deprecated public boolean clipRegion(Region region) { return clipRect(region.getBounds()); }

    public boolean getClipBounds(Rect bounds) {
        Matrix inverse = new Matrix();
        RectF r = new RectF(clip);
        if (matrix.invert(inverse)) inverse.mapRect(r);
        r.roundOut(bounds);
        return !bounds.isEmpty();
    }
    public final Rect getClipBounds() { Rect r = new Rect(); getClipBounds(r); return r; }

    public boolean quickReject(float left, float top, float right, float bottom) {
        RectF r = new RectF(left, top, right, bottom);
        matrix.mapRect(r);
        return !RectF.intersects(r, clip) || r.isEmpty() && (r.width() < 0 || r.height() < 0);
    }
    public boolean quickReject(float l, float t, float r, float b, EdgeType type) { return quickReject(l, t, r, b); }
    public boolean quickReject(RectF rect) { return quickReject(rect.left, rect.top, rect.right, rect.bottom); }
    public boolean quickReject(RectF rect, EdgeType type) { return quickReject(rect); }
    public boolean quickReject(Path path) { RectF b = new RectF(); path.computeBounds(b, false); return quickReject(b); }
    public boolean quickReject(Path path, EdgeType type) { return quickReject(path); }

    // ---- draws ----

    void writePath(Path p) {
        w.u8(p.getFillType().ordinal());
        w.i32(p.verbCount);
        w.i32(p.pointCount);
        w.bytes(p.verbs, 0, p.verbCount);
        for (int i = 0; i < p.pointCount; ++i) w.f32(p.points[i]);
    }

    public void drawColor(int color) { drawColor(color, BlendMode.SRC_OVER); }
    public void drawColor(long color) { drawColor(Color.toArgb(color), BlendMode.SRC_OVER); }
    public void drawColor(int color, PorterDuff.Mode mode) { drawColor(color, BlendMode.fromPorterDuff(mode)); }
    public void drawColor(int color, BlendMode mode) { w.u8(DisplayList.DRAW_COLOR); w.i32(color); w.u8(mode.skia()); }
    public void drawColor(long color, BlendMode mode) { drawColor(Color.toArgb(color), mode); }
    public void drawARGB(int a, int r, int g, int b) { drawColor(Color.argb(a, r, g, b)); }
    public void drawRGB(int r, int g, int b) { drawColor(Color.rgb(r, g, b)); }
    public void drawPaint(Paint paint) { w.u8(DisplayList.DRAW_PAINT); paint.write(w); }

    public void drawRect(float left, float top, float right, float bottom, Paint paint) {
        w.u8(DisplayList.DRAW_RECT); w.f32(left); w.f32(top); w.f32(right); w.f32(bottom); paint.write(w);
    }
    public void drawRect(RectF r, Paint paint) { drawRect(r.left, r.top, r.right, r.bottom, paint); }
    public void drawRect(Rect r, Paint paint) { drawRect(r.left, r.top, r.right, r.bottom, paint); }

    public void drawRoundRect(float left, float top, float right, float bottom, float rx, float ry, Paint paint) {
        w.u8(DisplayList.DRAW_RRECT); w.f32(left); w.f32(top); w.f32(right); w.f32(bottom); w.f32(rx); w.f32(ry); paint.write(w);
    }
    public void drawRoundRect(RectF r, float rx, float ry, Paint paint) { drawRoundRect(r.left, r.top, r.right, r.bottom, rx, ry, paint); }

    public void drawDoubleRoundRect(RectF outer, float outerRx, float outerRy, RectF inner, float innerRx, float innerRy, Paint paint) {
        drawDoubleRoundRect(outer, new float[] {outerRx, outerRy, outerRx, outerRy, outerRx, outerRy, outerRx, outerRy}, inner,
            new float[] {innerRx, innerRy, innerRx, innerRy, innerRx, innerRy, innerRx, innerRy}, paint);
    }
    public void drawDoubleRoundRect(RectF outer, float[] outerRadii, RectF inner, float[] innerRadii, Paint paint) {
        Path p = new Path();
        p.addRoundRect(outer, outerRadii, Path.Direction.CW);
        p.addRoundRect(inner, innerRadii, Path.Direction.CCW);
        p.setFillType(Path.FillType.EVEN_ODD);
        drawPath(p, paint);
    }

    public void drawOval(float left, float top, float right, float bottom, Paint paint) {
        w.u8(DisplayList.DRAW_OVAL); w.f32(left); w.f32(top); w.f32(right); w.f32(bottom); paint.write(w);
    }
    public void drawOval(RectF oval, Paint paint) { drawOval(oval.left, oval.top, oval.right, oval.bottom, paint); }
    public void drawCircle(float cx, float cy, float radius, Paint paint) {
        w.u8(DisplayList.DRAW_CIRCLE); w.f32(cx); w.f32(cy); w.f32(radius); paint.write(w);
    }
    public void drawArc(float left, float top, float right, float bottom, float startAngle, float sweepAngle, boolean useCenter, Paint paint) {
        w.u8(DisplayList.DRAW_ARC); w.f32(left); w.f32(top); w.f32(right); w.f32(bottom); w.f32(startAngle); w.f32(sweepAngle);
        w.u8(useCenter ? 1 : 0); paint.write(w);
    }
    public void drawArc(RectF oval, float startAngle, float sweepAngle, boolean useCenter, Paint paint) {
        drawArc(oval.left, oval.top, oval.right, oval.bottom, startAngle, sweepAngle, useCenter, paint);
    }

    public void drawLine(float startX, float startY, float stopX, float stopY, Paint paint) {
        drawLines(new float[] {startX, startY, stopX, stopY}, 0, 4, paint);
    }
    public void drawLines(float[] pts, int offset, int count, Paint paint) {
        count &= ~3;
        w.u8(DisplayList.DRAW_LINES); w.i32(count);
        for (int i = 0; i < count; ++i) w.f32(pts[offset + i]);
        paint.write(w);
    }
    public void drawLines(float[] pts, Paint paint) { drawLines(pts, 0, pts.length, paint); }
    public void drawPoint(float x, float y, Paint paint) { drawPoints(new float[] {x, y}, 0, 2, paint); }
    public void drawPoints(float[] pts, int offset, int count, Paint paint) {
        count &= ~1;
        w.u8(DisplayList.DRAW_POINTS); w.i32(count);
        for (int i = 0; i < count; ++i) w.f32(pts[offset + i]);
        paint.write(w);
    }
    public void drawPoints(float[] pts, Paint paint) { drawPoints(pts, 0, pts.length, paint); }

    public void drawPath(Path path, Paint paint) {
        if (path.isEmpty() && !path.isInverseFillType()) return;
        w.u8(DisplayList.DRAW_PATH); writePath(path); paint.write(w);
    }

    // ---- bitmaps ----

    public void drawBitmap(Bitmap bitmap, float left, float top, Paint paint) {
        drawBitmap(bitmap, null, new RectF(left, top, left + bitmap.getWidth(), top + bitmap.getHeight()), paint);
    }
    public void drawBitmap(Bitmap bitmap, Rect src, RectF dst, Paint paint) {
        list.addBitmap(bitmap);
        Rect s = src != null ? src : new Rect(0, 0, bitmap.getWidth(), bitmap.getHeight());
        w.u8(DisplayList.DRAW_BITMAP);
        w.i32(bitmap.id());
        w.f32(s.left); w.f32(s.top); w.f32(s.right); w.f32(s.bottom);
        w.f32(dst.left); w.f32(dst.top); w.f32(dst.right); w.f32(dst.bottom);
        (paint != null ? paint : DEFAULT_BITMAP_PAINT).write(w);
    }
    private static final Paint DEFAULT_BITMAP_PAINT = new Paint(Paint.FILTER_BITMAP_FLAG);
    public void drawBitmap(Bitmap bitmap, Rect src, Rect dst, Paint paint) { drawBitmap(bitmap, src, new RectF(dst), paint); }
    public void drawBitmap(Bitmap bitmap, Matrix m, Paint paint) {
        save();
        concat(m);
        drawBitmap(bitmap, 0, 0, paint);
        restore();
    }
    @Deprecated
    public void drawBitmap(int[] colors, int offset, int stride, float x, float y, int width, int height, boolean hasAlpha, Paint paint) {
        Bitmap b = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        b.setPixels(colors, offset, stride, 0, 0, width, height);
        drawBitmap(b, x, y, paint);
    }
    @Deprecated
    public void drawBitmap(int[] colors, int offset, int stride, int x, int y, int width, int height, boolean hasAlpha, Paint paint) {
        drawBitmap(colors, offset, stride, (float) x, (float) y, width, height, hasAlpha, paint);
    }
    public void drawBitmapMesh(Bitmap bitmap, int meshWidth, int meshHeight, float[] verts, int vertOffset, int[] colors, int colorOffset, Paint paint) {
        unsupported("drawBitmapMesh");
    }
    public void drawVertices(VertexMode mode, int vertexCount, float[] verts, int vertOffset, float[] texs, int texOffset,
                             int[] colors, int colorOffset, short[] indices, int indexOffset, int indexCount, Paint paint) {
        unsupported("drawVertices");
    }
    public void drawPatch(NinePatch patch, Rect dst, Paint paint) { patch.drawStretched(this, new RectF(dst), paint); }
    public void drawPatch(NinePatch patch, RectF dst, Paint paint) { patch.drawStretched(this, dst, paint); }

    private static final java.util.Set<String> WARNED = java.util.concurrent.ConcurrentHashMap.newKeySet();
    static void unsupported(String what) {
        if (WARNED.add(what)) android.util.Log.w("snx", "Canvas." + what + " draws nothing yet");
    }

    // ---- pictures and nodes ----

    public void drawPicture(Picture picture) {
        save();
        list.addChildList(picture.list());
        restore();
    }
    public void drawPicture(Picture picture, RectF dst) {
        save();
        translate(dst.left, dst.top);
        if (picture.getWidth() > 0 && picture.getHeight() > 0) scale(dst.width() / picture.getWidth(), dst.height() / picture.getHeight());
        drawPicture(picture);
        restore();
    }
    public void drawPicture(Picture picture, Rect dst) { drawPicture(picture, new RectF(dst)); }

    public void drawRenderNode(RenderNode renderNode) { list.addChild(renderNode); }

    // ---- text ----

    /** A run of text as glyphs at their positions, from (x, y) at the run's start: the paint's alignment applied. */
    private void glyphRun(char[] text, int start, int count, int ctxStart, int ctxEnd, float x, float y, boolean rtl,
                          Paint paint, boolean aligned) {
        if (count <= 0) return;
        int font = paint.font();
        int[] glyphs = new int[count * 2];
        float[] pos = new float[count * 4];
        float[] total = new float[1];
        int n = Text.shape(font, paint.getTextSize(), paint.getTextScaleX(), paint.getLetterSpacing(), text, start, count,
            ctxStart, ctxEnd, rtl, glyphs, pos, null, total);
        if (n > glyphs.length) {
            glyphs = new int[n];
            pos = new float[2 * n];
            Text.shape(font, paint.getTextSize(), paint.getTextScaleX(), paint.getLetterSpacing(), text, start, count,
                ctxStart, ctxEnd, rtl, glyphs, pos, null, total);
        }
        if (aligned) {
            if (paint.getTextAlign() == Paint.Align.CENTER) x -= total[0] / 2;
            else if (paint.getTextAlign() == Paint.Align.RIGHT) x -= total[0];
        }
        glyphs(font, paint, glyphs, pos, n, x, y);
        if (paint.isUnderlineText() || paint.isStrikeThruText()) {
            Paint line = new Paint(paint);
            line.setStyle(Paint.Style.FILL);
            float thick = Math.max(1, paint.getUnderlineThickness());
            if (paint.isUnderlineText()) {
                float uy = y + paint.getUnderlinePosition();
                drawRect(x, uy, x + total[0], uy + thick, line);
            }
            if (paint.isStrikeThruText()) {
                float sy = y + paint.getStrikeThruPosition();
                drawRect(x, sy, x + total[0], sy + thick, line);
            }
        }
    }

    private void glyphs(int font, Paint paint, int[] glyphs, float[] pos, int n, float x, float y) {
        if (n == 0) return;
        w.u8(DisplayList.DRAW_GLYPHS);
        w.i32(font);
        w.f32(paint.getTextSize());
        w.f32(paint.getTextScaleX());
        w.f32(paint.skew());
        w.u8(paint.fakeBold() ? 1 : 0);
        w.i32(n);
        for (int i = 0; i < n; ++i) w.i32(glyphs[i]);
        for (int i = 0; i < n; ++i) { w.f32(x + pos[2 * i]); w.f32(y + pos[2 * i + 1]); }
        paint.write(w);
    }

    private static char[] chars(CharSequence text, int start, int end) {
        char[] c = new char[end - start];
        android.text.TextUtils.getChars(text, start, end, c, 0);
        return c;
    }

    public void drawText(char[] text, int index, int count, float x, float y, Paint paint) {
        glyphRun(text, index, count, index, index + count, x, y, false, paint, true);
    }
    public void drawText(String text, float x, float y, Paint paint) { drawText(text.toCharArray(), 0, text.length(), x, y, paint); }
    public void drawText(String text, int start, int end, float x, float y, Paint paint) {
        drawText(chars(text, start, end), 0, end - start, x, y, paint);
    }
    public void drawText(CharSequence text, int start, int end, float x, float y, Paint paint) {
        drawText(chars(text, start, end), 0, end - start, x, y, paint);
    }
    public void drawTextRun(char[] text, int index, int count, int contextIndex, int contextCount, float x, float y,
                            boolean isRtl, Paint paint) {
        glyphRun(text, index, count, contextIndex, contextIndex + contextCount, x, y, isRtl, paint, true);
    }
    public void drawTextRun(CharSequence text, int start, int end, int contextStart, int contextEnd, float x, float y,
                            boolean isRtl, Paint paint) {
        char[] c = chars(text, contextStart, contextEnd);
        glyphRun(c, start - contextStart, end - start, 0, c.length, x, y, isRtl, paint, true);
    }
    public void drawTextRun(android.graphics.text.MeasuredText text, int start, int end, int contextStart, int contextEnd,
                            float x, float y, boolean isRtl, Paint paint) {
        drawTextRun(text.text(), start, end - start, contextStart, contextEnd - contextStart, x, y, isRtl, paint);
    }
    @Deprecated
    public void drawPosText(char[] text, int index, int count, float[] pos, Paint paint) {
        for (int i = 0; i < count; ++i) drawText(text, index + i, 1, pos[2 * i], pos[2 * i + 1], paint);
    }
    @Deprecated public void drawPosText(String text, float[] pos, Paint paint) { drawPosText(text.toCharArray(), 0, text.length(), pos, paint); }
    public void drawTextOnPath(char[] text, int index, int count, Path path, float hOffset, float vOffset, Paint paint) {
        PathMeasure m = new PathMeasure(path, false);
        float[] adv = new float[count], p = new float[2], tan = new float[2];
        paint.getTextWidths(text, index, count, adv);
        float d = hOffset;
        for (int i = 0; i < count; ++i) {
            if (!m.getPosTan(d + adv[i] / 2, p, tan)) break;
            save();
            translate(p[0], p[1]);
            rotate((float) Math.toDegrees(Math.atan2(tan[1], tan[0])));
            drawText(text, index + i, 1, -adv[i] / 2, vOffset, paint);
            restore();
            d += adv[i];
        }
    }
    public void drawTextOnPath(String text, Path path, float hOffset, float vOffset, Paint paint) {
        drawTextOnPath(text.toCharArray(), 0, text.length(), path, hOffset, vOffset, paint);
    }
    public void drawGlyphs(int[] glyphIds, int glyphIdOffset, float[] positions, int positionOffset, int glyphCount,
                           android.graphics.fonts.Font font, Paint paint) {
        int[] g = java.util.Arrays.copyOfRange(glyphIds, glyphIdOffset, glyphIdOffset + glyphCount);
        float[] p = java.util.Arrays.copyOfRange(positions, positionOffset, positionOffset + 2 * glyphCount);
        glyphs(font.handle(), paint, g, p, glyphCount, 0, 0);
    }
}
