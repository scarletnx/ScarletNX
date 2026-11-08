// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class and of PathParser (API level 36), with
// ScarletNX's changes:
// Copyright (C) 2014 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.graphics.drawable;

import android.content.res.ColorStateList;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PathMeasure;
import android.graphics.PixelFormat;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffColorFilter;
import android.graphics.RadialGradient;
import android.graphics.Rect;
import android.graphics.Shader;
import android.graphics.SweepGradient;
import android.util.AttributeSet;
import android.util.TypedValue;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;

/**
 * A drawable of vector paths, from a <vector> resource: groups (each a transform of its children), paths filled and
 * stroked with colors, color state lists or gradients, and clip paths, in a viewport scaled to the bounds. The tint
 * colors the whole drawable, as Android's does with its tint mode (src_in by default).
 */
public class VectorDrawable extends Drawable {
    // the attributes of <vector>, <group>, <path> and <clip-path>, by their android.R.attr ids
    private static final int[] VECTOR_ATTRS = {0x01010159, 0x01010155, 0x01010402, 0x01010403, 0x01010121, 0x0101031f,
        0x010103ea}; // width, height, viewportWidth, viewportHeight, tint, alpha, autoMirrored
    private static final int[] GROUP_ATTRS = {0x01010003, 0x01010326, 0x010101b5, 0x010101b6, 0x01010324, 0x01010325,
        0x0101045a, 0x0101045b}; // name, rotation, pivotX, pivotY, scaleX, scaleY, translateX, translateY
    private static final int[] PATH_ATTRS = {0x01010003, 0x01010405, 0x01010404, 0x01010406, 0x01010407, 0x01010408,
        0x01010409, 0x0101040a, 0x0101040b, 0x0101040c, 0x0101040d, 0x010104cb, 0x010104cc, 0x0101051e};
    private static final int P_NAME = 0, P_DATA = 1, P_FILL = 2, P_STROKE = 3, P_STROKE_WIDTH = 4, P_TRIM_START = 5,
        P_TRIM_END = 6, P_TRIM_OFFSET = 7, P_CAP = 8, P_JOIN = 9, P_MITER = 10, P_STROKE_ALPHA = 11, P_FILL_ALPHA = 12,
        P_FILL_TYPE = 13;
    private static final int[] CLIP_ATTRS = {0x01010003, 0x01010405}; // name, pathData

    private int width = -1, height = -1;
    private float viewportWidth, viewportHeight, rootAlpha = 1;
    private boolean autoMirrored;
    private final Group root = new Group();
    final HashMap<String, Object> targets = new HashMap<>();
    private ColorFilter filter;
    private ColorStateList tint;
    private PorterDuff.Mode tintMode = PorterDuff.Mode.SRC_IN;
    private PorterDuffColorFilter tintFilter;
    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG), strokePaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    public VectorDrawable() {
        strokePaint.setStyle(Paint.Style.STROKE);
    }

    // ---- the tree ----
    private static final class Group {
        String name;
        float rotation, pivotX, pivotY, scaleX = 1, scaleY = 1, translateX, translateY;
        final ArrayList<Object> children = new ArrayList<>();
    }
    private static final class VPath {
        String name;
        final Path path = new Path();
        boolean clip;
        ColorStateList fillColors, strokeColors;
        Shader fillShader, strokeShader;
        int fillColor, strokeColor;
        float strokeWidth, fillAlpha = 1, strokeAlpha = 1, trimStart, trimEnd = 1, trimOffset, miter = 4;
        Paint.Cap cap = Paint.Cap.BUTT;
        Paint.Join join = Paint.Join.MITER;
        Path.FillType fillType = Path.FillType.WINDING;
        Path trimmed;

        /** the path as trimmed: its first contour from start to end (shifted by the offset), as Android trims it */
        Path render() {
            if (trimStart == 0 && trimEnd == 1) return path;
            if (trimmed == null) trimmed = new Path();
            trimmed.reset();
            float start = (trimStart + trimOffset) % 1, end = (trimEnd + trimOffset) % 1;
            if (trimEnd - trimStart <= 0) return trimmed;
            PathMeasure m = new PathMeasure(path, false);
            float len = m.getLength();
            start *= len;
            end *= len;
            if (start > end) {
                m.getSegment(start, len, trimmed, true);
                m.getSegment(0, end, trimmed, true);
            } else {
                m.getSegment(start, end, trimmed, true);
            }
            trimmed.rLineTo(0, 0);
            trimmed.setFillType(fillType);
            return trimmed;
        }
    }

    // ---- inflation ----
    @Override public void inflate(Resources r, XmlPullParser parser, AttributeSet attrs, Resources.Theme theme)
            throws XmlPullParserException, IOException {
        super.inflate(r, parser, attrs, theme);
        TypedArray a = DrawableXml.obtain(r, theme, attrs, VECTOR_ATTRS);
        try {
            width = a.getDimensionPixelSize(0, -1);
            height = a.getDimensionPixelSize(1, -1);
            viewportWidth = a.getFloat(2, 0);
            viewportHeight = a.getFloat(3, 0);
            if (a.hasValue(4)) tint = a.getColorStateList(4);
            rootAlpha = a.getFloat(5, 1);
            autoMirrored = a.getBoolean(6, false);
        } finally {
            a.recycle();
        }
        if (viewportWidth <= 0 || viewportHeight <= 0)
            throw new XmlPullParserException(parser.getPositionDescription() + "<vector> tag requires viewportWidth and viewportHeight > 0");
        if (width <= 0 || height <= 0) throw new XmlPullParserException(parser.getPositionDescription() + "<vector> tag requires width and height > 0");
        inflateGroup(root, r, parser, attrs, theme);
        updateTint();
        updateColors(getState());
    }

    private void inflateGroup(Group g, Resources r, XmlPullParser parser, AttributeSet attrs, Resources.Theme theme)
            throws XmlPullParserException, IOException {
        int depth = parser.getDepth();
        while (DrawableXml.child(parser, depth)) {
            switch (parser.getName()) {
                case "group" -> {
                    Group child = new Group();
                    TypedArray a = DrawableXml.obtain(r, theme, attrs, GROUP_ATTRS);
                    try {
                        child.name = a.getString(0);
                        child.rotation = a.getFloat(1, 0);
                        child.pivotX = a.getFloat(2, 0);
                        child.pivotY = a.getFloat(3, 0);
                        child.scaleX = a.getFloat(4, 1);
                        child.scaleY = a.getFloat(5, 1);
                        child.translateX = a.getFloat(6, 0);
                        child.translateY = a.getFloat(7, 0);
                    } finally {
                        a.recycle();
                    }
                    if (child.name != null) targets.put(child.name, child);
                    g.children.add(child);
                    inflateGroup(child, r, parser, attrs, theme);
                }
                case "path" -> {
                    VPath p = inflatePath(r, attrs, theme);
                    g.children.add(p);
                    DrawableXml.skip(parser);
                }
                case "clip-path" -> {
                    VPath p = new VPath();
                    p.clip = true;
                    TypedArray a = DrawableXml.obtain(r, theme, attrs, CLIP_ATTRS);
                    try {
                        p.name = a.getString(0);
                        String data = a.getString(1);
                        if (data != null) parsePath(data, p.path);
                    } finally {
                        a.recycle();
                    }
                    if (p.name != null) targets.put(p.name, p);
                    g.children.add(p);
                    DrawableXml.skip(parser);
                }
                default -> DrawableXml.skip(parser);
            }
        }
    }

    private VPath inflatePath(Resources r, AttributeSet attrs, Resources.Theme theme) {
        VPath p = new VPath();
        TypedArray a = DrawableXml.obtain(r, theme, attrs, PATH_ATTRS);
        try {
            p.name = a.getString(P_NAME);
            String data = a.getString(P_DATA);
            if (data != null) parsePath(data, p.path);
            Object fill = color(r, theme, a, P_FILL);
            if (fill instanceof Shader s) p.fillShader = s;
            else p.fillColors = (ColorStateList) fill;
            Object stroke = color(r, theme, a, P_STROKE);
            if (stroke instanceof Shader s) p.strokeShader = s;
            else p.strokeColors = (ColorStateList) stroke;
            p.strokeWidth = a.getFloat(P_STROKE_WIDTH, 0);
            p.trimStart = a.getFloat(P_TRIM_START, 0);
            p.trimEnd = a.getFloat(P_TRIM_END, 1);
            p.trimOffset = a.getFloat(P_TRIM_OFFSET, 0);
            p.cap = switch (a.getInt(P_CAP, 0)) {
                case 1 -> Paint.Cap.ROUND;
                case 2 -> Paint.Cap.SQUARE;
                default -> Paint.Cap.BUTT;
            };
            p.join = switch (a.getInt(P_JOIN, 0)) {
                case 1 -> Paint.Join.ROUND;
                case 2 -> Paint.Join.BEVEL;
                default -> Paint.Join.MITER;
            };
            p.miter = a.getFloat(P_MITER, 4);
            p.strokeAlpha = a.getFloat(P_STROKE_ALPHA, 1);
            p.fillAlpha = a.getFloat(P_FILL_ALPHA, 1);
            p.fillType = a.getInt(P_FILL_TYPE, 0) == 1 ? Path.FillType.EVEN_ODD : Path.FillType.WINDING;
            p.path.setFillType(p.fillType);
        } finally {
            a.recycle();
        }
        if (p.name != null) targets.put(p.name, p);
        return p;
    }

    /**
     * A path's color attribute: a color state list, or a shader when it refers to a <gradient> color resource (the
     * complex colors of Android's ComplexColor), or null when absent
     */
    private static Object color(Resources r, Resources.Theme theme, TypedArray a, int index) {
        TypedValue v = a.peekValue(index);
        if (v == null || v.type == TypedValue.TYPE_NULL) return null;
        if (v.type >= TypedValue.TYPE_FIRST_COLOR_INT && v.type <= TypedValue.TYPE_LAST_COLOR_INT) return ColorStateList.valueOf(v.data);
        int id = v.resourceId;
        if (id != 0 && v.string != null && v.string.toString().endsWith(".xml")) {
            try (android.content.res.XmlResourceParser parser = r.getXml(id)) {
                int type;
                while ((type = parser.next()) != XmlPullParser.START_TAG && type != XmlPullParser.END_DOCUMENT) {}
                if (type == XmlPullParser.START_TAG && parser.getName().equals("gradient"))
                    return gradient(r, theme, parser, android.util.Xml.asAttributeSet(parser));
            } catch (XmlPullParserException | IOException | RuntimeException e) {
                // not a gradient: a color state list
            }
        }
        try {
            return a.getColorStateList(index);
        } catch (RuntimeException e) {
            return null;
        }
    }

    // startColor, centerColor, endColor, type, startX, startY, endX, endY, centerX, centerY, gradientRadius, tileMode
    private static final int[] GRADIENT_ATTRS = {0x0101019d, 0x0101020b, 0x0101019e, 0x010101a1, 0x01010510, 0x01010511,
        0x01010512, 0x01010513, 0x010101a2, 0x010101a3, 0x010101a4, 0x01010201};
    private static final int[] GRADIENT_ITEM_ATTRS = {0x010101a5, 0x01010514}; // color, offset

    /** a <gradient> color resource as a shader in the viewport's coordinates (Android's GradientColor) */
    private static Shader gradient(Resources r, Resources.Theme theme, XmlPullParser parser, AttributeSet attrs)
            throws XmlPullParserException, IOException {
        TypedArray a = DrawableXml.obtain(r, theme, attrs, GRADIENT_ATTRS);
        int start, end, type, tile;
        Integer center = null;
        float x0, y0, x1, y1, cx, cy, radius;
        try {
            start = a.getColor(0, 0);
            if (a.hasValue(1)) center = a.getColor(1, 0);
            end = a.getColor(2, 0);
            type = a.getInt(3, 0);
            x0 = a.getFloat(4, 0);
            y0 = a.getFloat(5, 0);
            x1 = a.getFloat(6, 0);
            y1 = a.getFloat(7, 0);
            cx = a.getFloat(8, 0);
            cy = a.getFloat(9, 0);
            radius = a.getFloat(10, 0);
            tile = a.getInt(11, 0);
        } finally {
            a.recycle();
        }
        ArrayList<int[]> items = new ArrayList<>();
        ArrayList<Float> offsets = new ArrayList<>();
        int depth = parser.getDepth();
        while (DrawableXml.child(parser, depth)) {
            if (parser.getName().equals("item")) {
                TypedArray i = DrawableXml.obtain(r, theme, attrs, GRADIENT_ITEM_ATTRS);
                try {
                    items.add(new int[] {i.getColor(0, 0)});
                    offsets.add(i.getFloat(1, 0));
                } finally {
                    i.recycle();
                }
            }
            DrawableXml.skip(parser);
        }
        int[] colors;
        float[] positions = null;
        if (!items.isEmpty()) {
            colors = new int[items.size()];
            positions = new float[items.size()];
            for (int k = 0; k < colors.length; ++k) {
                colors[k] = items.get(k)[0];
                positions[k] = offsets.get(k);
            }
            if (colors.length == 1) colors = new int[] {colors[0], colors[0]};
            if (positions.length != colors.length) positions = null;
        } else {
            colors = center != null ? new int[] {start, center, end} : new int[] {start, end};
        }
        Shader.TileMode mode = tile == 1 ? Shader.TileMode.REPEAT : tile == 2 ? Shader.TileMode.MIRROR : Shader.TileMode.CLAMP;
        return switch (type) {
            case 1 -> new RadialGradient(cx, cy, Math.max(radius, 1e-3f), colors, positions, mode);
            case 2 -> new SweepGradient(cx, cy, colors, positions);
            default -> new LinearGradient(x0, y0, x1, y1, colors, positions, mode);
        };
    }

    // ---- state and tint ----
    private boolean updateColors(int[] state) {
        return updateColors(root, state);
    }
    private static boolean updateColors(Group g, int[] state) {
        boolean changed = false;
        for (Object o : g.children) {
            if (o instanceof Group c) {
                changed |= updateColors(c, state);
            } else if (o instanceof VPath p) {
                int fill = p.fillColors != null ? p.fillColors.getColorForState(state, p.fillColors.getDefaultColor()) : 0;
                int stroke = p.strokeColors != null ? p.strokeColors.getColorForState(state, p.strokeColors.getDefaultColor()) : 0;
                changed |= fill != p.fillColor || stroke != p.strokeColor;
                p.fillColor = fill;
                p.strokeColor = stroke;
            }
        }
        return changed;
    }
    private boolean updateTint() {
        PorterDuffColorFilter old = tintFilter;
        tintFilter = tint != null && tintMode != null
            ? new PorterDuffColorFilter(tint.getColorForState(getState(), tint.getDefaultColor()), tintMode) : null;
        return old != tintFilter;
    }
    @Override public void setTintList(ColorStateList tint) {
        this.tint = tint;
        updateTint();
        invalidateSelf();
    }
    @Override public void setTintMode(PorterDuff.Mode tintMode) {
        this.tintMode = tintMode;
        updateTint();
        invalidateSelf();
    }
    @Override public boolean isStateful() { return (tint != null && tint.isStateful()) || stateful(root); }
    private static boolean stateful(Group g) {
        for (Object o : g.children) {
            if (o instanceof Group c && stateful(c)) return true;
            if (o instanceof VPath p && ((p.fillColors != null && p.fillColors.isStateful()) || (p.strokeColors != null && p.strokeColors.isStateful())))
                return true;
        }
        return false;
    }
    @Override protected boolean onStateChange(int[] state) {
        boolean changed = updateColors(state);
        if (tint != null && tint.isStateful()) changed |= updateTint();
        return changed;
    }

    // ---- drawing ----
    @Override public void draw(Canvas canvas) {
        Rect b = getBounds();
        if (b.width() <= 0 || b.height() <= 0 || viewportWidth <= 0 || viewportHeight <= 0) return;
        ColorFilter cf = filter != null ? filter : tintFilter;
        int save = canvas.save();
        canvas.clipRect(b);
        canvas.translate(b.left, b.top);
        if (autoMirrored && getLayoutDirection() == 1) {
            canvas.translate(b.width(), 0);
            canvas.scale(-1, 1);
        }
        canvas.scale(b.width() / viewportWidth, b.height() / viewportHeight);
        drawGroup(canvas, root, cf, rootAlpha * (getAlpha() / 255f));
        canvas.restoreToCount(save);
    }
    private void drawGroup(Canvas canvas, Group g, ColorFilter cf, float alpha) {
        int save = canvas.save();
        canvas.translate(g.translateX + g.pivotX, g.translateY + g.pivotY);
        canvas.rotate(g.rotation);
        canvas.scale(g.scaleX, g.scaleY);
        canvas.translate(-g.pivotX, -g.pivotY);
        for (Object o : g.children) {
            if (o instanceof Group c) {
                drawGroup(canvas, c, cf, alpha);
                continue;
            }
            VPath p = (VPath) o;
            if (p.clip) {
                canvas.clipPath(p.path);
                continue;
            }
            Path path = p.render();
            if (p.fillShader != null || (p.fillColor >>> 24) != 0) {
                fillPaint.setShader(p.fillShader);
                fillPaint.setColor(p.fillShader != null ? 0xff000000 : p.fillColor);
                fillPaint.setAlpha(Math.round((fillPaint.getAlpha()) * p.fillAlpha * alpha));
                fillPaint.setColorFilter(cf);
                path.setFillType(p.fillType);
                canvas.drawPath(path, fillPaint);
            }
            if (p.strokeWidth > 0 && (p.strokeShader != null || (p.strokeColor >>> 24) != 0)) {
                strokePaint.setShader(p.strokeShader);
                strokePaint.setColor(p.strokeShader != null ? 0xff000000 : p.strokeColor);
                strokePaint.setAlpha(Math.round((strokePaint.getAlpha()) * p.strokeAlpha * alpha));
                strokePaint.setColorFilter(cf);
                strokePaint.setStrokeWidth(p.strokeWidth);
                strokePaint.setStrokeCap(p.cap);
                strokePaint.setStrokeJoin(p.join);
                strokePaint.setStrokeMiter(p.miter);
                canvas.drawPath(path, strokePaint);
            }
        }
        canvas.restoreToCount(save);
    }

    private int alpha = 0xff;
    @Override public void setAlpha(int alpha) {
        if (this.alpha != alpha) {
            this.alpha = alpha;
            invalidateSelf();
        }
    }
    @Override public int getAlpha() { return alpha; }
    @Override public void setColorFilter(ColorFilter colorFilter) { filter = colorFilter; invalidateSelf(); }
    @Override public ColorFilter getColorFilter() { return filter; }
    @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    @Override public int getIntrinsicWidth() { return width; }
    @Override public int getIntrinsicHeight() { return height; }
    @Override public boolean isAutoMirrored() { return autoMirrored; }
    @Override public void setAutoMirrored(boolean mirrored) { if (autoMirrored != mirrored) { autoMirrored = mirrored; invalidateSelf(); } }
    @Override public Drawable mutate() { return this; }

    // ---- path data (Android's PathParser) ----
    /** SVG path data into the path: the commands M L H V C S Q T A Z, absolute and relative */
    static void parsePath(String d, Path path) {
        float[] cur = new float[6]; // x, y, last control x, y, subpath start x, y
        char prev = 'm';
        int i = 0, n = d.length();
        float[] args = new float[16];
        while (i < n) {
            char c = d.charAt(i);
            if (!Character.isLetter(c) || c == 'e' || c == 'E') {
                ++i;
                continue;
            }
            int count = 0;
            int j = i + 1;
            while (true) {
                // the numbers of the command: separated by spaces or commas, or by a sign or a second point
                while (j < n && (d.charAt(j) == ' ' || d.charAt(j) == ',' || d.charAt(j) == '\n' || d.charAt(j) == '\t' || d.charAt(j) == '\r')) ++j;
                if (j >= n) break;
                char s = d.charAt(j);
                if (!(s == '-' || s == '+' || s == '.' || Character.isDigit(s))) break;
                int start = j;
                boolean dot = false, exp = false;
                if (s == '-' || s == '+') ++j;
                while (j < n) {
                    char t = d.charAt(j);
                    if (Character.isDigit(t)) { ++j; continue; }
                    if (t == '.' && !dot && !exp) { dot = true; ++j; continue; }
                    if ((t == 'e' || t == 'E') && !exp) {
                        exp = true;
                        ++j;
                        if (j < n && (d.charAt(j) == '-' || d.charAt(j) == '+')) ++j;
                        continue;
                    }
                    break;
                }
                if (count == args.length) args = java.util.Arrays.copyOf(args, count * 2);
                try {
                    args[count++] = Float.parseFloat(d.substring(start, j));
                } catch (NumberFormatException e) {
                    --count;
                    ++j;
                }
                // an arc's flags may be written together ("a1 1 0 01 2 3"): one digit each
                if ((c == 'a' || c == 'A') && ((count - 1) % 7 == 3 || (count - 1) % 7 == 4) && j - start > 1) {
                    String num = d.substring(start, j);
                    if (num.length() > 1 && (num.charAt(0) == '0' || num.charAt(0) == '1') && Character.isDigit(num.charAt(1))) {
                        args[count - 1] = num.charAt(0) - '0';
                        j = start + 1;
                    }
                }
            }
            command(path, cur, prev, c, args, count);
            prev = c;
            i = j;
        }
    }

    private static void command(Path path, float[] cur, char prev, char cmd, float[] v, int count) {
        float x = cur[0], y = cur[1], cx = cur[2], cy = cur[3], sx = cur[4], sy = cur[5];
        int step = switch (cmd) {
            case 'z', 'Z' -> {
                path.close();
                x = cx = sx;
                y = cy = sy;
                path.moveTo(x, y);
                yield -1;
            }
            case 'm', 'M', 'l', 'L', 't', 'T' -> 2;
            case 'h', 'H', 'v', 'V' -> 1;
            case 'c', 'C' -> 6;
            case 's', 'S', 'q', 'Q' -> 4;
            case 'a', 'A' -> 7;
            default -> -1;
        };
        if (step < 0) {
            cur[0] = x; cur[1] = y; cur[2] = cx; cur[3] = cy; cur[4] = sx; cur[5] = sy;
            return;
        }
        if (count == 0 && step > 0) return;
        for (int k = 0; k + step <= count; k += step) {
            switch (cmd) {
                case 'm' -> {
                    x += v[k]; y += v[k + 1];
                    if (k > 0) path.lineTo(x, y);
                    else { path.moveTo(x, y); sx = x; sy = y; }
                }
                case 'M' -> {
                    x = v[k]; y = v[k + 1];
                    if (k > 0) path.lineTo(x, y);
                    else { path.moveTo(x, y); sx = x; sy = y; }
                }
                case 'l' -> { x += v[k]; y += v[k + 1]; path.lineTo(x, y); }
                case 'L' -> { x = v[k]; y = v[k + 1]; path.lineTo(x, y); }
                case 'h' -> { x += v[k]; path.lineTo(x, y); }
                case 'H' -> { x = v[k]; path.lineTo(x, y); }
                case 'v' -> { y += v[k]; path.lineTo(x, y); }
                case 'V' -> { y = v[k]; path.lineTo(x, y); }
                case 'c' -> {
                    path.cubicTo(x + v[k], y + v[k + 1], x + v[k + 2], y + v[k + 3], x + v[k + 4], y + v[k + 5]);
                    cx = x + v[k + 2]; cy = y + v[k + 3];
                    x += v[k + 4]; y += v[k + 5];
                }
                case 'C' -> {
                    path.cubicTo(v[k], v[k + 1], v[k + 2], v[k + 3], v[k + 4], v[k + 5]);
                    cx = v[k + 2]; cy = v[k + 3];
                    x = v[k + 4]; y = v[k + 5];
                }
                case 's', 'S' -> {
                    float rx = x, ry = y;
                    if ("cCsS".indexOf(prev) >= 0) { rx = 2 * x - cx; ry = 2 * y - cy; }
                    float x2 = cmd == 's' ? x + v[k] : v[k], y2 = cmd == 's' ? y + v[k + 1] : v[k + 1];
                    float x3 = cmd == 's' ? x + v[k + 2] : v[k + 2], y3 = cmd == 's' ? y + v[k + 3] : v[k + 3];
                    path.cubicTo(rx, ry, x2, y2, x3, y3);
                    cx = x2; cy = y2; x = x3; y = y3;
                }
                case 'q', 'Q' -> {
                    float x1 = cmd == 'q' ? x + v[k] : v[k], y1 = cmd == 'q' ? y + v[k + 1] : v[k + 1];
                    float x2 = cmd == 'q' ? x + v[k + 2] : v[k + 2], y2 = cmd == 'q' ? y + v[k + 3] : v[k + 3];
                    path.quadTo(x1, y1, x2, y2);
                    cx = x1; cy = y1; x = x2; y = y2;
                }
                case 't', 'T' -> {
                    float rx = x, ry = y;
                    if ("qQtT".indexOf(prev) >= 0) { rx = 2 * x - cx; ry = 2 * y - cy; }
                    float x2 = cmd == 't' ? x + v[k] : v[k], y2 = cmd == 't' ? y + v[k + 1] : v[k + 1];
                    path.quadTo(rx, ry, x2, y2);
                    cx = rx; cy = ry; x = x2; y = y2;
                }
                case 'a', 'A' -> {
                    float ex = cmd == 'a' ? x + v[k + 5] : v[k + 5], ey = cmd == 'a' ? y + v[k + 6] : v[k + 6];
                    arc(path, x, y, ex, ey, v[k], v[k + 1], v[k + 2], v[k + 3] != 0, v[k + 4] != 0);
                    x = cx = ex;
                    y = cy = ey;
                }
                default -> {}
            }
            prev = cmd;
        }
        if ("cCsSqQtT".indexOf(cmd) < 0) { cx = x; cy = y; }
        cur[0] = x; cur[1] = y; cur[2] = cx; cur[3] = cy; cur[4] = sx; cur[5] = sy;
    }

    /** An SVG arc from (x0, y0) to (x1, y1) as cubic Béziers, by its center (Android's PathParser.drawArc) */
    private static void arc(Path p, float x0, float y0, float x1, float y1, float a, float b, float theta, boolean large, boolean sweep) {
        double thetaD = Math.toRadians(theta), cosTheta = Math.cos(thetaD), sinTheta = Math.sin(thetaD);
        double x0p = (x0 * cosTheta + y0 * sinTheta) / a, y0p = (-x0 * sinTheta + y0 * cosTheta) / b;
        double x1p = (x1 * cosTheta + y1 * sinTheta) / a, y1p = (-x1 * sinTheta + y1 * cosTheta) / b;
        double dx = x0p - x1p, dy = y0p - y1p, xm = (x0p + x1p) / 2, ym = (y0p + y1p) / 2;
        double dsq = dx * dx + dy * dy;
        if (dsq == 0.0) return;
        double disc = 1.0 / dsq - 1.0 / 4.0;
        if (disc < 0.0) {
            float adjust = (float) (Math.sqrt(dsq) / 1.99999);
            arc(p, x0, y0, x1, y1, a * adjust, b * adjust, theta, large, sweep);
            return;
        }
        double s = Math.sqrt(disc), sdx = s * dx, sdy = s * dy, cx, cy;
        if (large == sweep) {
            cx = xm - sdy;
            cy = ym + sdx;
        } else {
            cx = xm + sdy;
            cy = ym - sdx;
        }
        double eta0 = Math.atan2(y0p - cy, x0p - cx), eta1 = Math.atan2(y1p - cy, x1p - cx), sweepAngle = eta1 - eta0;
        if (sweep != (sweepAngle >= 0)) sweepAngle += sweepAngle > 0 ? -2 * Math.PI : 2 * Math.PI;
        cx *= a;
        cy *= b;
        double tcx = cx;
        cx = cx * cosTheta - cy * sinTheta;
        cy = tcx * sinTheta + cy * cosTheta;
        arcToBezier(p, cx, cy, a, b, x0, y0, thetaD, eta0, sweepAngle);
    }
    private static void arcToBezier(Path p, double cx, double cy, double a, double b, double e1x, double e1y, double theta,
                                    double start, double sweep) {
        int segments = (int) Math.ceil(Math.abs(sweep * 4 / Math.PI));
        double eta1 = start, cosTheta = Math.cos(theta), sinTheta = Math.sin(theta), cosEta1 = Math.cos(eta1), sinEta1 = Math.sin(eta1);
        double ep1x = (-a * cosTheta * sinEta1) - (b * sinTheta * cosEta1), ep1y = (-a * sinTheta * sinEta1) + (b * cosTheta * cosEta1);
        double anglePerSegment = sweep / segments;
        for (int i = 0; i < segments; i++) {
            double eta2 = eta1 + anglePerSegment, sinEta2 = Math.sin(eta2), cosEta2 = Math.cos(eta2);
            double e2x = cx + (a * cosTheta * cosEta2) - (b * sinTheta * sinEta2), e2y = cy + (a * sinTheta * cosEta2) + (b * cosTheta * sinEta2);
            double ep2x = -a * cosTheta * sinEta2 - b * sinTheta * cosEta2, ep2y = -a * sinTheta * sinEta2 + b * cosTheta * cosEta2;
            double tanDiff2 = Math.tan((eta2 - eta1) / 2);
            double alpha = Math.sin(eta2 - eta1) * (Math.sqrt(4 + (3 * tanDiff2 * tanDiff2)) - 1) / 3;
            double q1x = e1x + alpha * ep1x, q1y = e1y + alpha * ep1y, q2x = e2x - alpha * ep2x, q2y = e2y - alpha * ep2y;
            p.rLineTo(0, 0);
            p.cubicTo((float) q1x, (float) q1y, (float) q2x, (float) q2y, (float) e2x, (float) e2y);
            eta1 = eta2;
            e1x = e2x;
            e1y = e2y;
            ep1x = ep2x;
            ep1y = ep2y;
        }
    }
}
