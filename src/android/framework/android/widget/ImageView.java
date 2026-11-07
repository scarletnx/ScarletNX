// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Matrix;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffColorFilter;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;
import android.net.Uri;
import android.util.AttributeSet;
import android.view.View;
import java.io.InputStream;

/** A view of a drawable, scaled into the view by its scale type. */
public class ImageView extends View {
    public enum ScaleType { MATRIX, FIT_XY, FIT_START, FIT_CENTER, FIT_END, CENTER, CENTER_CROP, CENTER_INSIDE }

    private Drawable drawable;
    private ScaleType scaleType = ScaleType.FIT_CENTER;
    private final Matrix matrix = new Matrix();
    private int level;
    private ColorStateList tint;
    private PorterDuff.Mode tintMode;
    private ColorFilter colorFilter;

    public ImageView(Context context) { super(context); }
    public ImageView(Context context, AttributeSet attrs) { super(context, attrs); }
    public ImageView(Context context, AttributeSet attrs, int defStyleAttr) { super(context, attrs, defStyleAttr); }
    public ImageView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) { super(context, attrs, defStyleAttr, defStyleRes); }

    public void setImageDrawable(Drawable drawable) {
        if (this.drawable != null && this.drawable != drawable) this.drawable.setCallback(null);
        this.drawable = drawable;
        if (drawable != null) {
            drawable.setCallback(this);
            drawable.setLevel(level);
            if (drawable.isStateful()) drawable.setState(getDrawableState());
            if (tint != null) drawable.setTintList(tint);
            if (tintMode != null) drawable.setTintMode(tintMode);
            if (colorFilter != null) drawable.setColorFilter(colorFilter);
        }
        requestLayout();
        invalidate();
    }
    public Drawable getDrawable() { return drawable; }
    public void setImageResource(int resId) { setImageDrawable(resId != 0 ? getContext().getDrawable(resId) : null); }
    public void setImageBitmap(android.graphics.Bitmap bm) { setImageDrawable(bm != null ? new android.graphics.drawable.BitmapDrawable(getResources(), bm) : null); }
    /** Reads a bitmap from a content, file or android.resource URI through the content resolver. */
    public void setImageURI(Uri uri) {
        Drawable d = null;
        if (uri != null) {
            try (InputStream in = getContext().getContentResolver().openInputStream(uri)) {
                android.graphics.Bitmap bm = in != null ? android.graphics.BitmapFactory.decodeStream(in) : null;
                if (bm != null) d = new android.graphics.drawable.BitmapDrawable(getResources(), bm);
            } catch (Exception e) {
                android.util.Log.w("ImageView", "Unable to open content: " + uri, e);
            }
        }
        setImageDrawable(d);
    }
    public void setImageLevel(int level) { this.level = level; if (drawable != null) { drawable.setLevel(level); invalidate(); } }
    public void setScaleType(ScaleType scaleType) {
        if (scaleType == null) throw new NullPointerException();
        this.scaleType = scaleType;
        requestLayout();
        invalidate();
    }
    public ScaleType getScaleType() { return scaleType; }
    public Matrix getImageMatrix() { return matrix; }
    public void setImageMatrix(Matrix matrix) { this.matrix.set(matrix); invalidate(); }
    public void setImageTintList(ColorStateList tint) { this.tint = tint; if (drawable != null) drawable.setTintList(tint); }
    public ColorStateList getImageTintList() { return tint; }
    public void setImageTintMode(PorterDuff.Mode tintMode) { this.tintMode = tintMode; if (drawable != null) drawable.setTintMode(tintMode); }
    public PorterDuff.Mode getImageTintMode() { return tintMode; }
    public final void setColorFilter(int color) { setColorFilter(color, PorterDuff.Mode.SRC_ATOP); }
    public final void setColorFilter(int color, PorterDuff.Mode mode) { setColorFilter(new PorterDuffColorFilter(color, mode)); }
    public void setColorFilter(ColorFilter cf) { colorFilter = cf; if (drawable != null) drawable.setColorFilter(cf); invalidate(); }
    public final void clearColorFilter() { setColorFilter(null); }
    public ColorFilter getColorFilter() { return colorFilter; }

    // View declares these in Android; until the View here has them, ImageView declares them.
    protected void onAnimationStart() {}
    protected void onAnimationEnd() {}

    @Override protected void drawableStateChanged() {
        super.drawableStateChanged();
        if (drawable != null && drawable.isStateful() && drawable.setState(getDrawableState())) invalidateDrawable(drawable);
    }
    @Override protected boolean verifyDrawable(Drawable dr) { return drawable == dr || super.verifyDrawable(dr); }
    @Override protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int w = drawable != null ? Math.max(1, drawable.getIntrinsicWidth()) : 0, h = drawable != null ? Math.max(1, drawable.getIntrinsicHeight()) : 0;
        w = Math.max(w + getPaddingLeft() + getPaddingRight(), getSuggestedMinimumWidth());
        h = Math.max(h + getPaddingTop() + getPaddingBottom(), getSuggestedMinimumHeight());
        setMeasuredDimension(resolveSizeAndState(w, widthMeasureSpec, 0), resolveSizeAndState(h, heightMeasureSpec, 0));
    }
    @Override protected void onDraw(Canvas canvas) {
        if (drawable == null) return;
        int vw = getWidth() - getPaddingLeft() - getPaddingRight(), vh = getHeight() - getPaddingTop() - getPaddingBottom();
        int dw = drawable.getIntrinsicWidth(), dh = drawable.getIntrinsicHeight();
        canvas.save();
        canvas.clipRect(getPaddingLeft() + getScrollX(), getPaddingTop() + getScrollY(), getPaddingLeft() + getScrollX() + vw, getPaddingTop() + getScrollY() + vh);
        canvas.translate(getPaddingLeft(), getPaddingTop());
        if (dw <= 0 || dh <= 0 || scaleType == ScaleType.FIT_XY) {
            drawable.setBounds(0, 0, vw, vh);
        } else {
            drawable.setBounds(0, 0, dw, dh);
            Matrix m = new Matrix();
            switch (scaleType) {
                case MATRIX -> m.set(matrix);
                case CENTER -> m.setTranslate(Math.round((vw - dw) * 0.5f), Math.round((vh - dh) * 0.5f));
                case CENTER_CROP -> {
                    float s = Math.max((float) vw / dw, (float) vh / dh);
                    m.setScale(s, s);
                    m.postTranslate(Math.round((vw - dw * s) * 0.5f), Math.round((vh - dh * s) * 0.5f));
                }
                case CENTER_INSIDE -> {
                    float s = Math.min(1f, Math.min((float) vw / dw, (float) vh / dh));
                    m.setScale(s, s);
                    m.postTranslate(Math.round((vw - dw * s) * 0.5f), Math.round((vh - dh * s) * 0.5f));
                }
                default -> m.setRectToRect(new RectF(0, 0, dw, dh), new RectF(0, 0, vw, vh),
                    scaleType == ScaleType.FIT_START ? Matrix.ScaleToFit.START : scaleType == ScaleType.FIT_END ? Matrix.ScaleToFit.END : Matrix.ScaleToFit.CENTER);
            }
            canvas.concat(m);
        }
        drawable.draw(canvas);
        canvas.restore();
    }
    @Override public CharSequence getAccessibilityClassName() { return ImageView.class.getName(); }
}
