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

    public void setShape(int shape) { this.shape = shape; invalidateSelf(); }
    public int getShape() { return shape; }
    public void setSize(int width, int height) { this.width = width; this.height = height; invalidateSelf(); }
    public void setColor(int argb) { setColor(ColorStateList.valueOf(argb)); }
    public void setColor(ColorStateList colorStateList) {
        solid = colorStateList;
        colors = null;
        shaderDirty = true;
        invalidateSelf();
    }
    public ColorStateList getColor() { return solid; }
    public void setColors(int[] colors) {
        this.colors = colors;
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
                return new RadialGradient(r.centerX(), r.centerY(), Math.max(gradientRadius < 1 ? gradientRadius * Math.min(r.width(), r.height()) : gradientRadius, 1e-3f), colors, null, Shader.TileMode.CLAMP);
            case SWEEP_GRADIENT:
                return new SweepGradient(r.centerX(), r.centerY(), colors, null);
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
                return new LinearGradient(x0, y0, x1, y1, colors, null, Shader.TileMode.CLAMP);
            }
        }
    }

    @Override protected void onBoundsChange(Rect bounds) { shaderDirty = true; }
    @Override protected boolean onStateChange(int[] state) {
        if (solid == null || !solid.isStateful()) return false;
        shaderDirty = true;
        return true;
    }
    @Override public boolean isStateful() { return solid != null && solid.isStateful(); }

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
