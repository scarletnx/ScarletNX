// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2007 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.widget;

import android.content.Context;
import android.content.res.Resources;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.View;

/**
 * A view of the selected item of an adapter. On Android a click opens a list of the items in a dialog or a drop-down
 * window. The runtime has no such window, so a click opens nothing and the selection stays.
 */
public class Spinner extends AbsSpinner {
    public static final int MODE_DIALOG = 0, MODE_DROPDOWN = 1;
    private final int mode;
    private int dropDownWidth = -2, dropDownHorizontalOffset, dropDownVerticalOffset, gravity = android.view.Gravity.CENTER;
    private Drawable popupBackground;
    private CharSequence prompt;

    public Spinner(Context context) { this(context, null); }
    public Spinner(Context context, int mode) { this(context, null, 0, 0, mode); }
    public Spinner(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public Spinner(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0, -1); }
    public Spinner(Context context, AttributeSet attrs, int defStyleAttr, int mode) { this(context, attrs, defStyleAttr, 0, mode); }
    public Spinner(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes, int mode) { this(context, attrs, defStyleAttr, defStyleRes, mode, null); }
    public Spinner(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes, int mode, Resources.Theme popupTheme) {
        super(context, attrs, defStyleAttr, defStyleRes);
        if (mode == -1) mode = attrs != null ? attrs.getAttributeIntValue("http://schemas.android.com/apk/res/android", "spinnerMode", MODE_DROPDOWN) : MODE_DROPDOWN;
        this.mode = mode;
        setFocusable(true);
        setClickable(true);
    }

    @Override public void setAdapter(SpinnerAdapter adapter) {
        if (adapter != null && adapter.getViewTypeCount() != 1) throw new IllegalArgumentException("Spinner adapter view type count must be 1");
        super.setAdapter(adapter);
    }
    public int getMode() { return mode; }
    public void setPopupBackgroundDrawable(Drawable background) { popupBackground = background; }
    public void setPopupBackgroundResource(int resId) { setPopupBackgroundDrawable(getContext().getDrawable(resId)); }
    public Drawable getPopupBackground() { return popupBackground; }
    public void setDropDownVerticalOffset(int pixels) { dropDownVerticalOffset = pixels; }
    public int getDropDownVerticalOffset() { return dropDownVerticalOffset; }
    public void setDropDownHorizontalOffset(int pixels) { dropDownHorizontalOffset = pixels; }
    public int getDropDownHorizontalOffset() { return dropDownHorizontalOffset; }
    public void setDropDownWidth(int pixels) { dropDownWidth = pixels; }
    public int getDropDownWidth() { return dropDownWidth; }
    public void setGravity(int gravity) { if (this.gravity != gravity) { this.gravity = gravity; requestLayout(); } }
    public int getGravity() { return gravity; }
    public void setPrompt(CharSequence prompt) { this.prompt = prompt; }
    public void setPromptId(int promptId) { setPrompt(getContext().getText(promptId)); }
    public CharSequence getPrompt() { return prompt; }
    @Override public int getBaseline() { View c = getSelectedView(); return c != null && c.getBaseline() >= 0 ? c.getTop() + c.getBaseline() : -1; }

    @Override public boolean performClick() {
        boolean handled = super.performClick();
        return handled || getCount() > 0;
    }
    @Override public boolean onTouchEvent(MotionEvent event) {
        if (!isEnabled()) return isClickable();
        if (event.getActionMasked() == MotionEvent.ACTION_DOWN) setPressed(true);
        else if (event.getActionMasked() == MotionEvent.ACTION_UP) { setPressed(false); performClick(); }
        else if (event.getActionMasked() == MotionEvent.ACTION_CANCEL) setPressed(false);
        return true;
    }

    @Override protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        View v = selectedItemView();
        int w = 0, h = 0;
        if (v != null) {
            measureChild(v, widthMeasureSpec, heightMeasureSpec);
            w = v.getMeasuredWidth();
            h = v.getMeasuredHeight();
        }
        w = Math.max(w + getPaddingLeft() + getPaddingRight(), getSuggestedMinimumWidth());
        h = Math.max(h + getPaddingTop() + getPaddingBottom(), getSuggestedMinimumHeight());
        setMeasuredDimension(resolveSizeAndState(w, widthMeasureSpec, 0), resolveSizeAndState(h, heightMeasureSpec, 0));
    }
    @Override protected void onLayout(boolean changed, int l, int t, int r, int b) {
        View v = getSelectedView();
        if (v == null) return;
        int w = v.getMeasuredWidth(), h = v.getMeasuredHeight();
        int inner = r - l - getPaddingLeft() - getPaddingRight();
        int x = getPaddingLeft();
        int hg = android.view.Gravity.getAbsoluteGravity(gravity, getLayoutDirection()) & android.view.Gravity.HORIZONTAL_GRAVITY_MASK;
        if (hg == android.view.Gravity.CENTER_HORIZONTAL) x += (inner - w) / 2; else if (hg == android.view.Gravity.RIGHT) x += inner - w;
        int y = getPaddingTop() + (b - t - getPaddingTop() - getPaddingBottom() - h) / 2;
        v.layout(x, y, x + w, y + h);
    }
    @Override public CharSequence getAccessibilityClassName() { return Spinner.class.getName(); }
}
