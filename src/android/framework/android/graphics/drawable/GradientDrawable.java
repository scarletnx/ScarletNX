// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.graphics.drawable;

import android.content.res.ColorStateList;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.LinearGradient;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.RadialGradient;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.SweepGradient;

/** A shape (rectangle, oval, line or ring) filled with a color or a gradient, with an optional stroke. */
public class GradientDrawable extends Drawable {
    public static final int RECTANGLE = 0, OVAL = 1, LINE = 2, RING = 3;
    public static final int LINEAR_GRADIENT = 0, RADIAL_GRADIENT = 1, SWEEP_GRADIENT = 2;

    public enum Orientation { TOP_BOTTOM, TR_BL, RIGHT_LEFT, BR_TL, BOTTOM_TOP, BL_TR, LEFT_RIGHT, TL_BR }

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private Paint stroke;
    private int shape = RECTANGLE, gradientType = LINEAR_GRADIENT, width = -1, height = -1, alpha = 0xff, baseColor;
    private Orientation orientation = Orientation.TOP_BOTTOM;
    private int[] colors;
    private float[] offsets; /* each color's position from 0 to 1, null for evenly spaced */
    private ColorStateList solid;
    private float radius, gradientRadius = 0.5f;
    private float[] radii;
    private ColorFilter filter;
    private boolean shaderDirty = true;
    private final RectF rect = new RectF();
    private final Path path = new Path();

    public GradientDrawable() { this(Orientation.TOP_BOTTOM, null); }
    public GradientDrawable(Orientation orientation, int[] colors) {
        this.orientation = orientation;
        this.colors = colors;
    }

    private Rect padding;
    private float centerX = 0.5f, centerY = 0.5f;

    // the attributes of <shape> and its children, by their android.R.attr ids
    private static final int[] SHAPE_ATTRS = {0x0101019a}; // shape
    private static final int[] SOLID_ATTRS = {0x010101a5}; // color
    private static final int[] STROKE_ATTRS = {0x01010159, 0x010101a5, 0x0101022a, 0x0101022b}; // width, color, dashWidth, dashGap
    private static final int[] CORNERS_ATTRS = {0x010101a8, 0x010101a9, 0x010101aa, 0x010101ab, 0x010101ac}; // radius, top left, top right, bottom left, bottom right
    private static final int[] PADDING_ATTRS = {0x010101ad, 0x010101ae, 0x010101af, 0x010101b0}; // left, top, right, bottom
    private static final int[] SIZE_ATTRS = {0x01010159, 0x01010155}; // width, height
    // startColor, centerColor, endColor, angle, type, centerX, centerY, gradientRadius
    private static final int[] GRADIENT_ATTRS = {0x0101019d, 0x0101020b, 0x0101019e, 0x010101a0, 0x010101a1, 0x010101a2, 0x010101a3, 0x010101a4};

    /** a <shape>: its kind, then its solid, stroke, corners, padding, size and gradient elements */
    @Override public void inflate(android.content.res.Resources r, org.xmlpull.v1.XmlPullParser parser, android.util.AttributeSet attrs,
                                  android.content.res.Resources.Theme theme) throws org.xmlpull.v1.XmlPullParserException, java.io.IOException {
        super.inflate(r, parser, attrs, theme);
        android.content.res.TypedArray a = DrawableXml.obtain(r, theme, attrs, SHAPE_ATTRS);
        try {
            shape = a.getInt(0, RECTANGLE);
        } finally {
            a.recycle();
        }
        int depth = parser.getDepth();
        while (DrawableXml.child(parser, depth)) {
            switch (parser.getName()) {
                case "solid" -> {
                    a = DrawableXml.obtain(r, theme, attrs, SOLID_ATTRS);
                    ColorStateList c = a.getColorStateList(0);
                    a.recycle();
                    setColor(c != null ? c : ColorStateList.valueOf(0));
                }
                case "stroke" -> {
                    a = DrawableXml.obtain(r, theme, attrs, STROKE_ATTRS);
                    int w = a.getDimensionPixelSize(0, 0);
                    ColorStateList c = a.getColorStateList(1);
                    float dashWidth = a.getDimension(2, 0), dashGap = a.getDimension(3, 0);
                    a.recycle();
                    strokeColors = c;
                    setStroke(w, c != null ? c.getColorForState(getState(), c.getDefaultColor()) : 0, dashWidth, dashGap);
                }
                case "corners" -> {
                    a = DrawableXml.obtain(r, theme, attrs, CORNERS_ATTRS);
                    float all = a.getDimensionPixelSize(0, 0);
                    float tl = a.getDimensionPixelSize(1, (int) all), tr = a.getDimensionPixelSize(2, (int) all);
                    float bl = a.getDimensionPixelSize(3, (int) all), br = a.getDimensionPixelSize(4, (int) all);
                    a.recycle();
                    if (tl == all && tr == all && bl == all && br == all) setCornerRadius(all);
                    else setCornerRadii(new float[] {tl, tl, tr, tr, br, br, bl, bl});
                }
                case "padding" -> {
                    a = DrawableXml.obtain(r, theme, attrs, PADDING_ATTRS);
                    padding = new Rect(a.getDimensionPixelOffset(0, 0), a.getDimensionPixelOffset(1, 0),
                        a.getDimensionPixelOffset(2, 0), a.getDimensionPixelOffset(3, 0));
                    a.recycle();
                }
                case "size" -> {
                    a = DrawableXml.obtain(r, theme, attrs, SIZE_ATTRS);
                    width = a.getDimensionPixelSize(0, -1);
                    height = a.getDimensionPixelSize(1, -1);
                    a.recycle();
                }
                case "gradient" -> {
                    a = DrawableXml.obtain(r, theme, attrs, GRADIENT_ATTRS);
                    int start = a.getColor(0, 0), end = a.getColor(2, 0);
                    colors = a.hasValue(1) ? new int[] {start, a.getColor(1, 0), end} : new int[] {start, end};
                    solid = null;
                    int angle = ((int) a.getFloat(3, 0) % 360 + 360) % 360;
                    orientation = switch (angle / 45) {
                        case 1 -> Orientation.BL_TR;
                        case 2 -> Orientation.BOTTOM_TOP;
                        case 3 -> Orientation.BR_TL;
                        case 4 -> Orientation.RIGHT_LEFT;
                        case 5 -> Orientation.TR_BL;
                        case 6 -> Orientation.TOP_BOTTOM;
                        case 7 -> Orientation.TL_BR;
                        default -> Orientation.LEFT_RIGHT;
                    };
                    gradientType = a.getInt(4, LINEAR_GRADIENT);
                    centerX = DrawableXml.dimensionOrFraction(a, 5, 1, 0.5f);
                    centerY = DrawableXml.dimensionOrFraction(a, 6, 1, 0.5f);
                    if (a.hasValue(7)) gradientRadius = DrawableXml.dimensionOrFraction(a, 7, 1, gradientRadius);
                    a.recycle();
                    shaderDirty = true;
                }
                default -> {}
            }
            DrawableXml.skip(parser);
        }
    }
    private ColorStateList strokeColors;
    /** a resource's, else one of a copy of the shape, its fill, stroke, corners, gradient, size and padding */
    @Override public ConstantState getConstantState() {
        if (fromResource()) return super.getConstantState();
        return new Copy() {
            @Override public Drawable newDrawable(android.content.res.Resources res, android.content.res.Resources.Theme theme) {
                GradientDrawable d = new GradientDrawable(orientation, colors != null ? colors.clone() : null);
                d.shape = shape;
                d.gradientType = gradientType;
                d.width = width;
                d.height = height;
                d.solid = solid;
                d.radius = radius;
                d.radii = radii != null ? radii.clone() : null;
                d.gradientRadius = gradientRadius;
                d.centerX = centerX;
                d.centerY = centerY;
                d.offsets = offsets != null ? offsets.clone() : null;
                d.padding = padding != null ? new Rect(padding) : null;
                d.strokeColors = strokeColors;
                if (stroke != null) {
                    d.stroke = new Paint(stroke);
                }
                d.alpha = alpha;
                return d;
            }
        };
    }
    @Override public boolean getPadding(Rect out) {
        if (padding == null) return super.getPadding(out);
        out.set(padding);
        return true;
    }

    public void setShape(int shape) { this.shape = shape; invalidateSelf(); }
    public int getShape() { return shape; }
    public void setSize(int width, int height) { this.width = width; this.height = height; invalidateSelf(); }
    public void setColor(int argb) { setColor(ColorStateList.valueOf(argb)); }
    public void setColor(ColorStateList colorStateList) {
        solid = colorStateList;
        colors = null;
        offsets = null;
        shaderDirty = true;
        invalidateSelf();
    }
    public ColorStateList getColor() { return solid; }
    public void setColors(int[] colors) { setColors(colors, null); }
    /** the gradient's colors, and each one's position from 0 to 1 (null for evenly spaced) */
    public void setColors(int[] colors, float[] offsets) {
        this.colors = colors;
        this.offsets = offsets;
        solid = null;
        shaderDirty = true;
        invalidateSelf();
    }
    public int[] getColors() { return colors != null ? colors.clone() : null; }
    public void setOrientation(Orientation orientation) { this.orientation = orientation; shaderDirty = true; invalidateSelf(); }
    public Orientation getOrientation() { return orientation; }
    public void setGradientType(int gradient) { gradientType = gradient; shaderDirty = true; invalidateSelf(); }
    public int getGradientType() { return gradientType; }
    public void setGradientRadius(float gradientRadius) { this.gradientRadius = gradientRadius; shaderDirty = true; invalidateSelf(); }
    public float getGradientRadius() { return gradientRadius; }
    /** the center of a radial or sweep gradient, as fractions of the bounds */
    public void setGradientCenter(float x, float y) { centerX = x; centerY = y; shaderDirty = true; invalidateSelf(); }
    public float getGradientCenterX() { return centerX; }
    public float getGradientCenterY() { return centerY; }
    public void setCornerRadius(float radius) {
        this.radius = Math.max(0, radius);
        radii = null;
        invalidateSelf();
    }
    public float getCornerRadius() { return radius; }
    public void setCornerRadii(float[] radii) {
        this.radii = radii != null ? radii.clone() : null;
        if (radii == null) radius = 0;
        invalidateSelf();
    }
    public float[] getCornerRadii() { return radii != null ? radii.clone() : null; }
    public void setStroke(int width, int color) { setStroke(width, color, 0, 0); }
    public void setStroke(int width, ColorStateList colorStateList) { setStroke(width, colorStateList.getColorForState(getState(), 0)); }
    public void setStroke(int width, int color, float dashWidth, float dashGap) {
        if (stroke == null) {
            stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
            stroke.setStyle(Paint.Style.STROKE);
        }
        stroke.setStrokeWidth(width);
        stroke.setColor(color);
        stroke.setPathEffect(dashWidth > 0 ? new android.graphics.DashPathEffect(new float[] {dashWidth, dashGap}, 0) : null);
        invalidateSelf();
    }

    private Shader gradient(RectF r) {
        if (colors == null || colors.length < 2) return null;
        switch (gradientType) {
            case RADIAL_GRADIENT:
                return new RadialGradient(r.left + centerX * r.width(), r.top + centerY * r.height(), Math.max(gradientRadius < 1 ? gradientRadius * Math.min(r.width(), r.height()) : gradientRadius, 1e-3f), colors, offsets, Shader.TileMode.CLAMP);
            case SWEEP_GRADIENT:
                return new SweepGradient(r.left + centerX * r.width(), r.top + centerY * r.height(), colors, offsets);
            default: {
                float x0, y0, x1, y1;
                switch (orientation) {
                    case TR_BL: x0 = r.right; y0 = r.top; x1 = r.left; y1 = r.bottom; break;
                    case RIGHT_LEFT: x0 = r.right; y0 = r.top; x1 = r.left; y1 = r.top; break;
                    case BR_TL: x0 = r.right; y0 = r.bottom; x1 = r.left; y1 = r.top; break;
                    case BOTTOM_TOP: x0 = r.left; y0 = r.bottom; x1 = r.left; y1 = r.top; break;
                    case BL_TR: x0 = r.left; y0 = r.bottom; x1 = r.right; y1 = r.top; break;
                    case LEFT_RIGHT: x0 = r.left; y0 = r.top; x1 = r.right; y1 = r.top; break;
                    case TL_BR: x0 = r.left; y0 = r.top; x1 = r.right; y1 = r.bottom; break;
                    default: x0 = r.left; y0 = r.top; x1 = r.left; y1 = r.bottom; break;
                }
                return new LinearGradient(x0, y0, x1, y1, colors, offsets, Shader.TileMode.CLAMP);
            }
        }
    }

    @Override protected void onBoundsChange(Rect bounds) { shaderDirty = true; }
    @Override protected boolean onStateChange(int[] state) {
        boolean changed = false;
        if (strokeColors != null && strokeColors.isStateful() && stroke != null) {
            int c = strokeColors.getColorForState(state, strokeColors.getDefaultColor());
            if (c != stroke.getColor()) { stroke.setColor(c); changed = true; }
        }
        if (solid == null || !solid.isStateful()) return changed;
        shaderDirty = true;
        return true;
    }
    @Override public boolean isStateful() { return (solid != null && solid.isStateful()) || (strokeColors != null && strokeColors.isStateful()); }

    @Override public void draw(Canvas canvas) {
        float inset = stroke != null ? stroke.getStrokeWidth() / 2 : 0;
        rect.set(getBounds());
        rect.inset(inset, inset);
        if (shaderDirty) {
            fill.setShader(gradient(rect));
            baseColor = solid != null ? solid.getColorForState(getState(), 0) : colors == null || colors.length == 0 ? 0
                : colors.length == 1 ? colors[0] : 0xff000000;
            shaderDirty = false;
        }
        fill.setColor(baseColor);
        fill.setAlpha((baseColor >>> 24) * alpha / 255);
        fill.setColorFilter(filter);
        if (stroke != null) stroke.setColorFilter(filter);
        boolean hasFill = fill.getShader() != null || (fill.getColor() >>> 24) != 0;
        switch (shape) {
            case OVAL:
                if (hasFill) canvas.drawOval(rect, fill);
                if (stroke != null) canvas.drawOval(rect, stroke);
                break;
            case LINE:
                if (stroke != null) canvas.drawLine(rect.left, rect.centerY(), rect.right, rect.centerY(), stroke);
                break;
            case RING: {
                float outer = Math.min(rect.width(), rect.height()) / 2, inner = outer / 3;
                path.reset();
                path.addCircle(rect.centerX(), rect.centerY(), outer, Path.Direction.CW);
                path.addCircle(rect.centerX(), rect.centerY(), inner, Path.Direction.CCW);
                if (hasFill) canvas.drawPath(path, fill);
                if (stroke != null) canvas.drawPath(path, stroke);
                break;
            }
            default:
                if (radii != null) {
                    path.reset();
                    path.addRoundRect(rect, radii, Path.Direction.CW);
                    if (hasFill) canvas.drawPath(path, fill);
                    if (stroke != null) canvas.drawPath(path, stroke);
                } else if (radius > 0) {
                    float r = Math.min(radius, Math.min(rect.width(), rect.height()) / 2);
                    if (hasFill) canvas.drawRoundRect(rect, r, r, fill);
                    if (stroke != null) canvas.drawRoundRect(rect, r, r, stroke);
                } else {
                    if (hasFill) canvas.drawRect(rect, fill);
                    if (stroke != null) canvas.drawRect(rect, stroke);
                }
        }
    }
    @Override public void setAlpha(int alpha) { this.alpha = alpha; invalidateSelf(); }
    @Override public int getAlpha() { return alpha; }
    @Override public void setColorFilter(ColorFilter colorFilter) { filter = colorFilter; invalidateSelf(); }
    @Override public ColorFilter getColorFilter() { return filter; }
    @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    @Override public int getIntrinsicWidth() { return width; }
    @Override public int getIntrinsicHeight() { return height; }
    @Override public void getOutline(Outline outline) {
        Rect b = getBounds();
        if (shape == OVAL) outline.setOval(b);
        else outline.setRoundRect(b, radii == null ? radius : 0);
        outline.setAlpha(alpha / 255f);
    }
}
