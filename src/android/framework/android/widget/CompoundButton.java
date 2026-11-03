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

/** A button with a checked state, which a click toggles, and a button drawable at its left. */
public abstract class CompoundButton extends Button implements Checkable {
    private static final int[] CHECKED_STATE_SET = {0x010100a0}; // android.R.attr.state_checked
    private boolean checked, broadcasting;
    private Drawable buttonDrawable;
    private ColorStateList buttonTint;
    private PorterDuff.Mode buttonTintMode;
    private OnCheckedChangeListener onCheckedChangeListener;
    OnCheckedChangeListener widgetListener;

    public interface OnCheckedChangeListener { void onCheckedChanged(CompoundButton buttonView, boolean isChecked); }

    public CompoundButton(Context context) { this(context, null); }
    public CompoundButton(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public CompoundButton(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public CompoundButton(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        setGravity(Gravity.CENTER_VERTICAL | Gravity.START);
    }

    @Override public void toggle() { setChecked(!checked); }
    @Override public boolean performClick() {
        toggle();
        boolean handled = super.performClick();
        if (!handled) playSoundEffect(android.view.SoundEffectConstants.CLICK);
        return handled;
    }
    @Override public boolean isChecked() { return checked; }
    @Override public void setChecked(boolean checked) {
        if (this.checked == checked) return;
        this.checked = checked;
        refreshDrawableState();
        if (broadcasting) return;
        broadcasting = true;
        if (onCheckedChangeListener != null) onCheckedChangeListener.onCheckedChanged(this, checked);
        if (widgetListener != null) widgetListener.onCheckedChanged(this, checked);
        broadcasting = false;
    }
    public void setOnCheckedChangeListener(OnCheckedChangeListener listener) { onCheckedChangeListener = listener; }

    public void setButtonDrawable(int resId) { setButtonDrawable(resId != 0 ? getContext().getDrawable(resId) : null); }
    public void setButtonDrawable(Drawable drawable) {
        if (buttonDrawable == drawable) return;
        if (buttonDrawable != null) { buttonDrawable.setCallback(null); unscheduleDrawable(buttonDrawable); }
        buttonDrawable = drawable;
        if (drawable != null) {
            drawable.setCallback(this);
            if (drawable.isStateful()) drawable.setState(getDrawableState());
            if (buttonTint != null) drawable.setTintList(buttonTint);
            if (buttonTintMode != null) drawable.setTintMode(buttonTintMode);
            setMinHeight(drawable.getIntrinsicHeight());
        }
        requestLayout();
        invalidate();
    }
    private void setMinHeight(int h) { if (h > 0) setMinimumHeight(h); }
    public Drawable getButtonDrawable() { return buttonDrawable; }
    public void setButtonTintList(ColorStateList tint) { buttonTint = tint; if (buttonDrawable != null) buttonDrawable.setTintList(tint); }
    public ColorStateList getButtonTintList() { return buttonTint; }
    public void setButtonTintMode(PorterDuff.Mode tintMode) { buttonTintMode = tintMode; if (buttonDrawable != null) buttonDrawable.setTintMode(tintMode); }
    public PorterDuff.Mode getButtonTintMode() { return buttonTintMode; }

    @Override public int getCompoundPaddingLeft() {
        int padding = super.getCompoundPaddingLeft();
        return buttonDrawable != null ? padding + Math.max(0, buttonDrawable.getIntrinsicWidth()) : padding;
    }
    @Override protected int[] onCreateDrawableState(int extraSpace) {
        int[] state = super.onCreateDrawableState(extraSpace + 1);
        if (checked) mergeDrawableStates(state, CHECKED_STATE_SET);
        return state;
    }
    @Override protected void drawableStateChanged() {
        super.drawableStateChanged();
        if (buttonDrawable != null && buttonDrawable.isStateful() && buttonDrawable.setState(getDrawableState())) invalidateDrawable(buttonDrawable);
    }
    @Override public void drawableHotspotChanged(float x, float y) { super.drawableHotspotChanged(x, y); if (buttonDrawable != null) buttonDrawable.setHotspot(x, y); }
    @Override protected boolean verifyDrawable(Drawable who) { return super.verifyDrawable(who) || who == buttonDrawable; }
    @Override public void jumpDrawablesToCurrentState() { super.jumpDrawablesToCurrentState(); if (buttonDrawable != null) buttonDrawable.jumpToCurrentState(); }
    @Override protected void onDraw(Canvas canvas) {
        Drawable d = buttonDrawable;
        if (d != null) {
            int h = Math.max(0, d.getIntrinsicHeight()), w = Math.max(0, d.getIntrinsicWidth());
            int g = getGravity() & Gravity.VERTICAL_GRAVITY_MASK;
            int top = g == Gravity.BOTTOM ? getHeight() - h : g == Gravity.CENTER_VERTICAL ? (getHeight() - h) / 2 : 0;
            d.setBounds(0, top, w, top + h);
            d.draw(canvas);
        }
        super.onDraw(canvas);
    }
    @Override public CharSequence getAccessibilityClassName() { return CompoundButton.class.getName(); }
}
