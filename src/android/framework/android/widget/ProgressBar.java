// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Canvas;
import android.graphics.drawable.Animatable;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;
import android.view.View;

/** A bar of progress between a minimum and a maximum, or an indeterminate one with no value. */
public class ProgressBar extends View {
    private static final int MAX_LEVEL = 10000;
    private int min, max = 100, progress, secondaryProgress;
    private boolean indeterminate, onlyIndeterminate;
    private Drawable progressDrawable, indeterminateDrawable;
    private ColorStateList progressTint, indeterminateTint;

    public ProgressBar(Context context) { this(context, null); }
    public ProgressBar(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public ProgressBar(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public ProgressBar(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        if (attrs != null) {
            String ns = "http://schemas.android.com/apk/res/android";
            max = attrs.getAttributeIntValue(ns, "max", max);
            progress = attrs.getAttributeIntValue(ns, "progress", 0);
            indeterminate = attrs.getAttributeBooleanValue(ns, "indeterminate", false);
            onlyIndeterminate = attrs.getAttributeBooleanValue(ns, "indeterminateOnly", false);
        }
    }

    public synchronized boolean isIndeterminate() { return indeterminate; }
    public synchronized void setIndeterminate(boolean indeterminate) {
        if ((!onlyIndeterminate || indeterminate) && indeterminate != this.indeterminate) {
            this.indeterminate = indeterminate;
            animateIndeterminate();
            invalidate();
        }
    }
    private void animateIndeterminate() {
        if (indeterminateDrawable instanceof Animatable a) { if (indeterminate && isAttachedToWindow()) a.start(); else a.stop(); }
    }
    public Drawable getIndeterminateDrawable() { return indeterminateDrawable; }
    public void setIndeterminateDrawable(Drawable d) {
        if (indeterminateDrawable == d) return;
        if (indeterminateDrawable != null) indeterminateDrawable.setCallback(null);
        indeterminateDrawable = d;
        if (d != null) { d.setCallback(this); if (indeterminateTint != null) d.setTintList(indeterminateTint); }
        animateIndeterminate();
        requestLayout();
        invalidate();
    }
    public Drawable getProgressDrawable() { return progressDrawable; }
    public void setProgressDrawable(Drawable d) {
        if (progressDrawable == d) return;
        if (progressDrawable != null) progressDrawable.setCallback(null);
        progressDrawable = d;
        if (d != null) { d.setCallback(this); if (progressTint != null) d.setTintList(progressTint); refreshLevel(); }
        requestLayout();
        invalidate();
    }
    public void setProgressTintList(ColorStateList tint) { progressTint = tint; if (progressDrawable != null) progressDrawable.setTintList(tint); }
    public ColorStateList getProgressTintList() { return progressTint; }
    public void setIndeterminateTintList(ColorStateList tint) { indeterminateTint = tint; if (indeterminateDrawable != null) indeterminateDrawable.setTintList(tint); }
    public ColorStateList getIndeterminateTintList() { return indeterminateTint; }

    public synchronized void setProgress(int progress) { setProgressInternal(progress, false); }
    public void setProgress(int progress, boolean animate) { setProgressInternal(progress, false); }
    synchronized boolean setProgressInternal(int progress, boolean fromUser) {
        if (indeterminate) return false;
        progress = Math.max(min, Math.min(max, progress));
        if (progress == this.progress) return false;
        this.progress = progress;
        refreshLevel();
        onProgressRefresh(fromUser);
        return true;
    }
    /** Tells a subclass that the progress changed, and if the user changed it. */
    void onProgressRefresh(boolean fromUser) {}
    public synchronized void setSecondaryProgress(int secondaryProgress) {
        if (indeterminate) return;
        this.secondaryProgress = Math.max(min, Math.min(max, secondaryProgress));
        refreshLevel();
    }
    public synchronized int getProgress() { return indeterminate ? 0 : progress; }
    public synchronized int getSecondaryProgress() { return indeterminate ? 0 : secondaryProgress; }
    public synchronized int getMin() { return min; }
    public synchronized int getMax() { return max; }
    public synchronized void setMin(int min) { this.min = Math.min(min, max); if (progress < this.min) setProgressInternal(this.min, false); refreshLevel(); }
    public synchronized void setMax(int max) {
        this.max = Math.max(max, min);
        if (progress > this.max) progress = this.max;
        if (secondaryProgress > this.max) secondaryProgress = this.max;
        refreshLevel();
    }
    public final synchronized void incrementProgressBy(int diff) { setProgress(progress + diff); }
    public final synchronized void incrementSecondaryProgressBy(int diff) { setSecondaryProgress(secondaryProgress + diff); }
    /** The drawable level of the progress, from 0 at the minimum to 10000 at the maximum. */
    int level(int value) { int range = max - min; return range > 0 ? (int) ((long) (value - min) * MAX_LEVEL / range) : 0; }
    private void refreshLevel() {
        if (progressDrawable != null) progressDrawable.setLevel(level(progress));
        invalidate();
    }

    @Override protected void onAttachedToWindow() { super.onAttachedToWindow(); animateIndeterminate(); }
    @Override protected void onDetachedFromWindow() { if (indeterminateDrawable instanceof Animatable a) a.stop(); super.onDetachedFromWindow(); }
    @Override protected boolean verifyDrawable(Drawable who) { return who == progressDrawable || who == indeterminateDrawable || super.verifyDrawable(who); }
    @Override protected void drawableStateChanged() {
        super.drawableStateChanged();
        int[] state = getDrawableState();
        if (progressDrawable != null && progressDrawable.isStateful()) progressDrawable.setState(state);
        if (indeterminateDrawable != null && indeterminateDrawable.isStateful()) indeterminateDrawable.setState(state);
    }
    Drawable current() { return indeterminate ? indeterminateDrawable : progressDrawable; }
    @Override protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        Drawable d = current();
        int w = d != null ? Math.max(0, d.getIntrinsicWidth()) : 0, h = d != null ? Math.max(0, d.getIntrinsicHeight()) : 0;
        w += getPaddingLeft() + getPaddingRight();
        h += getPaddingTop() + getPaddingBottom();
        setMeasuredDimension(resolveSizeAndState(Math.max(w, getSuggestedMinimumWidth()), widthMeasureSpec, 0),
            resolveSizeAndState(Math.max(h, getSuggestedMinimumHeight()), heightMeasureSpec, 0));
    }
    @Override protected synchronized void onDraw(Canvas canvas) {
        Drawable d = current();
        if (d == null) return;
        d.setBounds(getPaddingLeft(), getPaddingTop(), getWidth() - getPaddingRight(), getHeight() - getPaddingBottom());
        d.draw(canvas);
    }
    @Override public CharSequence getAccessibilityClassName() { return ProgressBar.class.getName(); }
}
