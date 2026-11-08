// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Canvas;
import android.graphics.PorterDuff;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;
import android.view.Gravity;

/** A text view with a checked state and a check mark drawable at its end, for the items of a choice list. */
public class CheckedTextView extends TextView implements Checkable {
    private static final int[] CHECKED_STATE_SET = {0x010100a0}; // android.R.attr.state_checked
    private boolean checked;
    private Drawable checkMark;
    private ColorStateList checkMarkTint;
    private PorterDuff.Mode checkMarkTintMode;

    public CheckedTextView(Context context) { this(context, null); }
    public CheckedTextView(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public CheckedTextView(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public CheckedTextView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) { super(context, attrs, defStyleAttr, defStyleRes); }

    @Override public void toggle() { setChecked(!checked); }
    @Override public boolean isChecked() { return checked; }
    @Override public void setChecked(boolean checked) { if (this.checked != checked) { this.checked = checked; refreshDrawableState(); } }

    public void setCheckMarkDrawable(int resId) { setCheckMarkDrawable(resId != 0 ? getContext().getDrawable(resId) : null); }
    public void setCheckMarkDrawable(Drawable d) {
        if (checkMark != null) checkMark.setCallback(null);
        checkMark = d;
        if (d != null) {
            d.setCallback(this);
            if (d.isStateful()) d.setState(getDrawableState());
            if (checkMarkTint != null) d.setTintList(checkMarkTint);
            if (checkMarkTintMode != null) d.setTintMode(checkMarkTintMode);
            if (d.getIntrinsicHeight() > 0) setMinimumHeight(d.getIntrinsicHeight());
        }
        requestLayout();
        invalidate();
    }
    public Drawable getCheckMarkDrawable() { return checkMark; }
    public void setCheckMarkTintList(ColorStateList tint) { checkMarkTint = tint; if (checkMark != null) checkMark.setTintList(tint); }
    public ColorStateList getCheckMarkTintList() { return checkMarkTint; }
    public void setCheckMarkTintMode(PorterDuff.Mode tintMode) { checkMarkTintMode = tintMode; if (checkMark != null) checkMark.setTintMode(tintMode); }
    public PorterDuff.Mode getCheckMarkTintMode() { return checkMarkTintMode; }

    @Override public int getCompoundPaddingRight() {
        int padding = super.getCompoundPaddingRight();
        return checkMark != null ? padding + Math.max(0, checkMark.getIntrinsicWidth()) : padding;
    }
    @Override protected int[] onCreateDrawableState(int extraSpace) {
        int[] state = super.onCreateDrawableState(extraSpace + 1);
        if (checked) mergeDrawableStates(state, CHECKED_STATE_SET);
        return state;
    }
    @Override protected void drawableStateChanged() {
        super.drawableStateChanged();
        if (checkMark != null && checkMark.isStateful() && checkMark.setState(getDrawableState())) invalidateDrawable(checkMark);
    }
    @Override protected boolean verifyDrawable(Drawable who) { return who == checkMark || super.verifyDrawable(who); }
    @Override public void jumpDrawablesToCurrentState() { super.jumpDrawablesToCurrentState(); if (checkMark != null) checkMark.jumpToCurrentState(); }
    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (checkMark == null) return;
        int w = Math.max(0, checkMark.getIntrinsicWidth()), h = Math.max(0, checkMark.getIntrinsicHeight());
        int g = getGravity() & Gravity.VERTICAL_GRAVITY_MASK;
        int top = g == Gravity.BOTTOM ? getHeight() - h : g == Gravity.CENTER_VERTICAL ? (getHeight() - h) / 2 : 0;
        int right = getWidth() - getPaddingRight();
        checkMark.setBounds(right - w, top, right, top + h);
        checkMark.draw(canvas);
    }
    @Override public CharSequence getAccessibilityClassName() { return CheckedTextView.class.getName(); }
}
