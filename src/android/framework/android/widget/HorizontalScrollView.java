// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2009 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.widget;

import android.content.Context;
import android.graphics.Rect;
import android.util.AttributeSet;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;

/** A frame of one child that scrolls horizontally when the child is wider than the frame. */
public class HorizontalScrollView extends FrameLayout {
    private boolean fillViewport, smoothScrolling = true;
    private final ScrollDrag drag;

    public HorizontalScrollView(Context context) { this(context, null); }
    public HorizontalScrollView(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public HorizontalScrollView(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public HorizontalScrollView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        setFocusable(true);
        setDescendantFocusability(FOCUS_AFTER_DESCENDANTS);
        setWillNotDraw(false);
        if (attrs != null) fillViewport = attrs.getAttributeBooleanValue("http://schemas.android.com/apk/res/android", "fillViewport", false);
        drag = new ScrollDrag(this, new ScrollDrag.Target() {
            @Override public int position() { return getScrollX(); }
            @Override public int range() { return getScrollRange(); }
            @Override public void scrollToPosition(int position) { scrollTo(position, getScrollY()); }
        }, false);
    }

    private static void one(ViewGroup g) { if (g.getChildCount() > 0) throw new IllegalStateException("HorizontalScrollView can host only one direct child"); }
    @Override public void addView(View child) { one(this); super.addView(child); }
    @Override public void addView(View child, int index) { one(this); super.addView(child, index); }
    @Override public void addView(View child, ViewGroup.LayoutParams params) { one(this); super.addView(child, params); }
    @Override public void addView(View child, int index, ViewGroup.LayoutParams params) { one(this); super.addView(child, index, params); }

    public boolean isFillViewport() { return fillViewport; }
    public void setFillViewport(boolean fillViewport) { if (this.fillViewport != fillViewport) { this.fillViewport = fillViewport; requestLayout(); } }
    public boolean isSmoothScrollingEnabled() { return smoothScrolling; }
    public void setSmoothScrollingEnabled(boolean smoothScrollingEnabled) { smoothScrolling = smoothScrollingEnabled; }
    public int getMaxScrollAmount() { return (int) (0.5f * getWidth()); }
    private int viewport() { return getWidth() - getPaddingLeft() - getPaddingRight(); }
    private int getScrollRange() {
        if (getChildCount() == 0) return 0;
        View c = getChildAt(0);
        MarginLayoutParams lp = (MarginLayoutParams) c.getLayoutParams();
        return Math.max(0, c.getWidth() + lp.leftMargin + lp.rightMargin - viewport());
    }

    /** The child gets the height of the frame, and the width that it wants. */
    @Override protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int w = 0, h = 0, state = 0;
        View c = getChildCount() > 0 ? getChildAt(0) : null;
        if (c != null && c.getVisibility() != GONE) {
            MarginLayoutParams lp = (MarginLayoutParams) c.getLayoutParams();
            int hs = getChildMeasureSpec(heightMeasureSpec, getPaddingTop() + getPaddingBottom() + lp.topMargin + lp.bottomMargin, lp.height);
            c.measure(MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED), hs);
            w = c.getMeasuredWidth() + lp.leftMargin + lp.rightMargin;
            h = c.getMeasuredHeight() + lp.topMargin + lp.bottomMargin;
            state = c.getMeasuredState();
        }
        w = Math.max(w + getPaddingLeft() + getPaddingRight(), getSuggestedMinimumWidth());
        h = Math.max(h + getPaddingTop() + getPaddingBottom(), getSuggestedMinimumHeight());
        setMeasuredDimension(resolveSizeAndState(w, widthMeasureSpec, 0), resolveSizeAndState(h, heightMeasureSpec, state << MEASURED_HEIGHT_STATE_SHIFT));
        if (c == null || !fillViewport || MeasureSpec.getMode(widthMeasureSpec) == MeasureSpec.UNSPECIFIED) return;
        MarginLayoutParams lp = (MarginLayoutParams) c.getLayoutParams();
        int width = getMeasuredWidth() - getPaddingLeft() - getPaddingRight() - lp.leftMargin - lp.rightMargin;
        if (c.getMeasuredWidth() < width) {
            int hs = getChildMeasureSpec(heightMeasureSpec, getPaddingTop() + getPaddingBottom() + lp.topMargin + lp.bottomMargin, lp.height);
            c.measure(MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY), hs);
        }
    }
    @Override protected void onLayout(boolean changed, int l, int t, int r, int b) {
        super.onLayout(changed, l, t, r, b);
        scrollTo(getScrollX(), getScrollY());
    }
    @Override public void scrollTo(int x, int y) {
        if (getChildCount() == 0) { super.scrollTo(x, y); return; }
        super.scrollTo(Math.max(0, Math.min(getScrollRange(), x)), y);
    }
    public final void smoothScrollBy(int dx, int dy) { if (smoothScrolling) drag.smoothScrollBy(dx); else scrollBy(dx, dy); }
    public final void smoothScrollTo(int x, int y) { smoothScrollBy(x - getScrollX(), y - getScrollY()); }
    public void fling(int velocityX) { drag.fling(velocityX); }
    public boolean fullScroll(int direction) { return scrollToEdge(direction == FOCUS_RIGHT ? getScrollRange() : 0); }
    public boolean pageScroll(int direction) { return scrollToEdge(getScrollX() + (direction == FOCUS_RIGHT ? viewport() : -viewport())); }
    public boolean arrowScroll(int direction) { return scrollToEdge(getScrollX() + (direction == FOCUS_RIGHT ? getMaxScrollAmount() / 5 : -getMaxScrollAmount() / 5)); }
    private boolean scrollToEdge(int x) {
        int target = Math.max(0, Math.min(getScrollRange(), x));
        if (target == getScrollX()) return false;
        smoothScrollTo(target, getScrollY());
        return true;
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event) { return super.dispatchKeyEvent(event) || executeKeyEvent(event); }
    public boolean executeKeyEvent(KeyEvent event) {
        if (event.getAction() != KeyEvent.ACTION_DOWN) return false;
        switch (event.getKeyCode()) {
            case KeyEvent.KEYCODE_DPAD_LEFT: return event.isAltPressed() ? fullScroll(FOCUS_LEFT) : arrowScroll(FOCUS_LEFT);
            case KeyEvent.KEYCODE_DPAD_RIGHT: return event.isAltPressed() ? fullScroll(FOCUS_RIGHT) : arrowScroll(FOCUS_RIGHT);
            case KeyEvent.KEYCODE_SPACE: return pageScroll(event.isShiftPressed() ? FOCUS_LEFT : FOCUS_RIGHT);
            default: return false;
        }
    }
    @Override public boolean onInterceptTouchEvent(MotionEvent ev) { return drag.intercept(ev); }
    @Override public boolean onTouchEvent(MotionEvent ev) { return drag.touch(ev); }
    @Override protected int computeHorizontalScrollRange() {
        if (getChildCount() == 0) return viewport();
        return Math.max(viewport(), getChildAt(0).getRight());
    }
    @Override protected int computeHorizontalScrollOffset() { return Math.max(0, super.computeHorizontalScrollOffset()); }
    @Override public boolean requestChildRectangleOnScreen(View child, Rect rectangle, boolean immediate) {
        int left = child.getLeft() + rectangle.left - getScrollX(), right = child.getLeft() + rectangle.right - getScrollX();
        int delta = left < 0 ? left : right > viewport() ? Math.min(right - viewport(), left) : 0;
        if (delta == 0) return false;
        if (immediate) scrollBy(delta, 0); else smoothScrollBy(delta, 0);
        return true;
    }
    @Override public CharSequence getAccessibilityClassName() { return HorizontalScrollView.class.getName(); }
}
