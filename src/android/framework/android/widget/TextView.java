// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.graphics.Canvas;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextPaint;
import android.util.AttributeSet;
import android.view.View;

/** A view of text: one StaticLayout of it. */
public class TextView extends View {
    public enum BufferType { NORMAL, SPANNABLE, EDITABLE }

    private CharSequence text = "";
    private final TextPaint paint = new TextPaint(android.graphics.Paint.ANTI_ALIAS_FLAG);
    private Layout layout;

    public TextView(Context context) {
        super(context);
        paint.setTextSize(14 * context.getResources().getDisplayMetrics().scaledDensity);
        paint.density = context.getResources().getDisplayMetrics().density;
    }
    public TextView(Context context, AttributeSet attrs) { this(context); }
    public TextView(Context context, AttributeSet attrs, int defStyleAttr) { this(context); }

    public void setText(CharSequence text) { setText(text, BufferType.NORMAL); }
    public void setText(CharSequence text, BufferType type) { this.text = text != null ? text : ""; layout = null; requestLayout(); invalidate(); }
    public void setText(int resid) { setText(getContext().getText(resid)); }
    public CharSequence getText() { return text; }
    public int length() { return text.length(); }
    public void setTextColor(int color) { paint.setColor(color); invalidate(); }
    public void setTextSize(float size) { paint.setTextSize(size * getResources().getDisplayMetrics().scaledDensity); layout = null; requestLayout(); }
    public void setTextSize(int unit, float size) { paint.setTextSize(android.util.TypedValue.applyDimension(unit, size, getResources().getDisplayMetrics())); layout = null; requestLayout(); }
    public float getTextSize() { return paint.getTextSize(); }
    public TextPaint getPaint() { return paint; }
    public final Layout getLayout() { return layout; }
    private android.text.method.MovementMethod movementMethod;
    public void setMovementMethod(android.text.method.MovementMethod movement) { movementMethod = movement; }
    public final android.text.method.MovementMethod getMovementMethod() { return movementMethod; }
    public void setGravity(int gravity) {}
    public void setSingleLine(boolean singleLine) {}
    public void setMaxLines(int maxLines) {}
    public void setEllipsize(android.text.TextUtils.TruncateAt where) {}
    public void setTypeface(android.graphics.Typeface tf) { paint.setTypeface(tf); layout = null; requestLayout(); }
    public void setLinkTextColor(int color) { paint.linkColor = color; }
    @Override protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int wmode = MeasureSpec.getMode(widthMeasureSpec), wsize = MeasureSpec.getSize(widthMeasureSpec);
        int avail = wmode == MeasureSpec.UNSPECIFIED ? Integer.MAX_VALUE / 4 : Math.max(0, wsize - getPaddingLeft() - getPaddingRight());
        layout = StaticLayout.Builder.obtain(text, 0, text.length(), paint, avail).build();
        int w = 0;
        for (int i = 0; i < layout.getLineCount(); ++i) w = Math.max(w, (int) Math.ceil(layout.getLineWidth(i)));
        w += getPaddingLeft() + getPaddingRight();
        int h = layout.getHeight() + getPaddingTop() + getPaddingBottom();
        setMeasuredDimension(resolveSize(w, widthMeasureSpec), resolveSize(h, heightMeasureSpec));
    }
    @Override protected void onDraw(Canvas canvas) {
        if (layout == null) return;
        canvas.save();
        canvas.translate(getPaddingLeft(), getPaddingTop());
        layout.draw(canvas);
        canvas.restore();
    }
}
