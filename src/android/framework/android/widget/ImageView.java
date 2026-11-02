// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;
import android.view.View;

/** A view of a drawable. */
public class ImageView extends View {
    public enum ScaleType { MATRIX, FIT_XY, FIT_START, FIT_CENTER, FIT_END, CENTER, CENTER_CROP, CENTER_INSIDE }

    private Drawable drawable;
    private ScaleType scaleType = ScaleType.FIT_CENTER;

    public ImageView(Context context) { super(context); }
    public ImageView(Context context, AttributeSet attrs) { super(context, attrs); }
    public ImageView(Context context, AttributeSet attrs, int defStyleAttr) { super(context, attrs, defStyleAttr); }

    public void setImageDrawable(Drawable drawable) {
        this.drawable = drawable;
        if (drawable != null) drawable.setCallback(this);
        requestLayout();
        invalidate();
    }
    public Drawable getDrawable() { return drawable; }
    public void setImageResource(int resId) { setImageDrawable(resId != 0 ? getContext().getDrawable(resId) : null); }
    public void setImageBitmap(android.graphics.Bitmap bm) { setImageDrawable(bm != null ? new android.graphics.drawable.BitmapDrawable(getResources(), bm) : null); }
    public void setScaleType(ScaleType scaleType) { this.scaleType = scaleType; invalidate(); }
    public ScaleType getScaleType() { return scaleType; }
    @Override protected void onDraw(Canvas canvas) {
        if (drawable == null) return;
        drawable.setBounds(getPaddingLeft(), getPaddingTop(), getWidth() - getPaddingRight(), getHeight() - getPaddingBottom());
        drawable.draw(canvas);
    }
}
