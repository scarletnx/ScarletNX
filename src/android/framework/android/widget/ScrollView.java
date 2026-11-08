// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.widget;

import android.content.Context;
import android.graphics.Rect;
import android.util.AttributeSet;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;

/** A frame of one child that scrolls vertically when the child is taller than the frame. */
public class ScrollView extends FrameLayout {
    private boolean fillViewport, smoothScrolling = true;
    private final ScrollDrag drag;

    public ScrollView(Context context) { this(context, null); }
    public ScrollView(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public ScrollView(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public ScrollView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        setFocusable(true);
        setDescendantFocusability(FOCUS_AFTER_DESCENDANTS);
        setWillNotDraw(false);
        if (attrs != null) fillViewport = attrs.getAttributeBooleanValue("http://schemas.android.com/apk/res/android", "fillViewport", false);
        drag = new ScrollDrag(this, new ScrollDrag.Target() {
            @Override public int position() { return getScrollY(); }
            @Override public int range() { return getScrollRange(); }
            @Override public void scrollToPosition(int position) { scrollTo(getScrollX(), position); }
        }, true);
    }

    private static void one(ViewGroup g) { if (g.getChildCount() > 0) throw new IllegalStateException("ScrollView can host only one direct child"); }
    @Override public void addView(View child) { one(this); super.addView(child); }
    @Override public void addView(View child, int index) { one(this); super.addView(child, index); }
    @Override public void addView(View child, ViewGroup.LayoutParams params) { one(this); super.addView(child, params); }
    @Override public void addView(View child, int index, ViewGroup.LayoutParams params) { one(this); super.addView(child, index, params); }

    public boolean isFillViewport() { return fillViewport; }
    public void setFillViewport(boolean fillViewport) { if (this.fillViewport != fillViewport) { this.fillViewport = fillViewport; requestLayout(); } }
    public boolean isSmoothScrollingEnabled() { return smoothScrolling; }
    public void setSmoothScrollingEnabled(boolean smoothScrollingEnabled) { smoothScrolling = smoothScrollingEnabled; }
    public int getMaxScrollAmount() { return (int) (0.5f * getHeight()); }
    private int viewport() { return getHeight() - getPaddingTop() - getPaddingBottom(); }
    private int getScrollRange() {
        if (getChildCount() == 0) return 0;
        View c = getChildAt(0);
        MarginLayoutParams lp = (MarginLayoutParams) c.getLayoutParams();
        return Math.max(0, c.getHeight() + lp.topMargin + lp.bottomMargin - viewport());
    }

    /** The child gets the width of the frame, and the height that it wants. */
    @Override protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int w = 0, h = 0, state = 0;
        View c = getChildCount() > 0 ? getChildAt(0) : null;
        if (c != null && c.getVisibility() != GONE) {
            MarginLayoutParams lp = (MarginLayoutParams) c.getLayoutParams();
            int ws = getChildMeasureSpec(widthMeasureSpec, getPaddingLeft() + getPaddingRight() + lp.leftMargin + lp.rightMargin, lp.width);
            c.measure(ws, MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED));
            w = c.getMeasuredWidth() + lp.leftMargin + lp.rightMargin;
            h = c.getMeasuredHeight() + lp.topMargin + lp.bottomMargin;
            state = c.getMeasuredState();
        }
        w = Math.max(w + getPaddingLeft() + getPaddingRight(), getSuggestedMinimumWidth());
        h = Math.max(h + getPaddingTop() + getPaddingBottom(), getSuggestedMinimumHeight());
        setMeasuredDimension(resolveSizeAndState(w, widthMeasureSpec, state), resolveSizeAndState(h, heightMeasureSpec, 0));
        if (c == null || !fillViewport || MeasureSpec.getMode(heightMeasureSpec) == MeasureSpec.UNSPECIFIED) return;
        MarginLayoutParams lp = (MarginLayoutParams) c.getLayoutParams();
        int height = getMeasuredHeight() - getPaddingTop() - getPaddingBottom() - lp.topMargin - lp.bottomMargin;
        if (c.getMeasuredHeight() < height) {
            int ws = getChildMeasureSpec(widthMeasureSpec, getPaddingLeft() + getPaddingRight() + lp.leftMargin + lp.rightMargin, lp.width);
            c.measure(ws, MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY));
        }
    }
    @Override protected void onLayout(boolean changed, int l, int t, int r, int b) {
        super.onLayout(changed, l, t, r, b);
        scrollTo(getScrollX(), getScrollY());
    }
    @Override public void scrollTo(int x, int y) {
        if (getChildCount() == 0) { super.scrollTo(x, y); return; }
        super.scrollTo(x, Math.max(0, Math.min(getScrollRange(), y)));
    }
    public final void smoothScrollBy(int dx, int dy) { if (smoothScrolling) drag.smoothScrollBy(dy); else scrollBy(dx, dy); }
    public final void smoothScrollTo(int x, int y) { smoothScrollBy(x - getScrollX(), y - getScrollY()); }
    public void fling(int velocityY) { drag.fling(velocityY); }
    public boolean fullScroll(int direction) { return scrollToEdge(direction == FOCUS_DOWN ? getScrollRange() : 0); }
    public boolean pageScroll(int direction) { return scrollToEdge(getScrollY() + (direction == FOCUS_DOWN ? viewport() : -viewport())); }
    public boolean arrowScroll(int direction) { return scrollToEdge(getScrollY() + (direction == FOCUS_DOWN ? getMaxScrollAmount() / 5 : -getMaxScrollAmount() / 5)); }
    private boolean scrollToEdge(int y) {
        int target = Math.max(0, Math.min(getScrollRange(), y));
        if (target == getScrollY()) return false;
        smoothScrollTo(getScrollX(), target);
        return true;
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event) { return super.dispatchKeyEvent(event) || executeKeyEvent(event); }
    public boolean executeKeyEvent(KeyEvent event) {
        if (event.getAction() != KeyEvent.ACTION_DOWN) return false;
        switch (event.getKeyCode()) {
            case KeyEvent.KEYCODE_DPAD_UP: return event.isAltPressed() ? fullScroll(FOCUS_UP) : arrowScroll(FOCUS_UP);
            case KeyEvent.KEYCODE_DPAD_DOWN: return event.isAltPressed() ? fullScroll(FOCUS_DOWN) : arrowScroll(FOCUS_DOWN);
            case KeyEvent.KEYCODE_SPACE: return pageScroll(event.isShiftPressed() ? FOCUS_UP : FOCUS_DOWN);
            default: return false;
        }
    }
    @Override public boolean onInterceptTouchEvent(MotionEvent ev) { return drag.intercept(ev); }
    @Override public boolean onTouchEvent(MotionEvent ev) { return drag.touch(ev); }
    @Override protected int computeVerticalScrollRange() {
        if (getChildCount() == 0) return viewport();
        return Math.max(viewport(), getChildAt(0).getBottom());
    }
    @Override protected int computeVerticalScrollOffset() { return Math.max(0, super.computeVerticalScrollOffset()); }
    @Override public boolean requestChildRectangleOnScreen(View child, Rect rectangle, boolean immediate) {
        int top = child.getTop() + rectangle.top - getScrollY(), bottom = child.getTop() + rectangle.bottom - getScrollY();
        int delta = top < 0 ? top : bottom > viewport() ? Math.min(bottom - viewport(), top) : 0;
        if (delta == 0) return false;
        if (immediate) scrollBy(0, delta); else smoothScrollBy(0, delta);
        return true;
    }
    @Override public CharSequence getAccessibilityClassName() { return ScrollView.class.getName(); }
}
