// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.util.AttributeSet;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;

/** Children stacked over each other, each placed by its gravity. */
public class FrameLayout extends ViewGroup {
    private boolean measureAllChildren;

    public FrameLayout(Context context) { super(context); }
    public FrameLayout(Context context, AttributeSet attrs) { super(context, attrs); }
    public FrameLayout(Context context, AttributeSet attrs, int defStyleAttr) { super(context, attrs, defStyleAttr); }
    public FrameLayout(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) { super(context, attrs, defStyleAttr, defStyleRes); }

    public static class LayoutParams extends MarginLayoutParams {
        public static final int UNSPECIFIED_GRAVITY = -1;
        public int gravity = UNSPECIFIED_GRAVITY;

        public LayoutParams(Context c, AttributeSet attrs) { super(c, attrs); }
        public LayoutParams(int width, int height) { super(width, height); }
        public LayoutParams(int width, int height, int gravity) { super(width, height); this.gravity = gravity; }
        public LayoutParams(ViewGroup.LayoutParams source) { super(source); }
        public LayoutParams(MarginLayoutParams source) { super(source); }
        public LayoutParams(LayoutParams source) { super(source); gravity = source.gravity; }
    }

    public void setMeasureAllChildren(boolean measureAll) { measureAllChildren = measureAll; }
    public boolean getMeasureAllChildren() { return measureAllChildren; }
    public void setForegroundGravity(int foregroundGravity) {}

    @Override protected LayoutParams generateDefaultLayoutParams() { return new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT); }
    @Override protected boolean checkLayoutParams(ViewGroup.LayoutParams p) { return p instanceof LayoutParams; }
    @Override protected ViewGroup.LayoutParams generateLayoutParams(ViewGroup.LayoutParams lp) {
        if (lp instanceof LayoutParams l) return new LayoutParams(l);
        if (lp instanceof MarginLayoutParams m) return new LayoutParams(m);
        return new LayoutParams(lp);
    }
    @Override public LayoutParams generateLayoutParams(AttributeSet attrs) { return new LayoutParams(getContext(), attrs); }
    @Override public boolean shouldDelayChildPressedState() { return false; }

    @Override protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int count = getChildCount(), maxWidth = 0, maxHeight = 0, state = 0;
        for (int i = 0; i < count; ++i) {
            View c = getChildAt(i);
            if (!measureAllChildren && c.getVisibility() == GONE) continue;
            measureChildWithMargins(c, widthMeasureSpec, 0, heightMeasureSpec, 0);
            LayoutParams lp = (LayoutParams) c.getLayoutParams();
            maxWidth = Math.max(maxWidth, c.getMeasuredWidth() + lp.leftMargin + lp.rightMargin);
            maxHeight = Math.max(maxHeight, c.getMeasuredHeight() + lp.topMargin + lp.bottomMargin);
            state = combineMeasuredStates(state, c.getMeasuredState());
        }
        maxWidth += getPaddingLeft() + getPaddingRight();
        maxHeight += getPaddingTop() + getPaddingBottom();
        maxWidth = Math.max(maxWidth, getSuggestedMinimumWidth());
        maxHeight = Math.max(maxHeight, getSuggestedMinimumHeight());
        setMeasuredDimension(resolveSizeAndState(maxWidth, widthMeasureSpec, state),
            resolveSizeAndState(maxHeight, heightMeasureSpec, state << MEASURED_HEIGHT_STATE_SHIFT));
        // children that match the parent's size get it exactly, once it is known
        for (int i = 0; i < count; ++i) {
            View c = getChildAt(i);
            if (c.getVisibility() == GONE) continue;
            LayoutParams lp = (LayoutParams) c.getLayoutParams();
            if (lp.width != LayoutParams.MATCH_PARENT && lp.height != LayoutParams.MATCH_PARENT) continue;
            int w = lp.width == LayoutParams.MATCH_PARENT
                ? MeasureSpec.makeMeasureSpec(Math.max(0, getMeasuredWidth() - getPaddingLeft() - getPaddingRight() - lp.leftMargin - lp.rightMargin), MeasureSpec.EXACTLY)
                : getChildMeasureSpec(widthMeasureSpec, getPaddingLeft() + getPaddingRight() + lp.leftMargin + lp.rightMargin, lp.width);
            int h = lp.height == LayoutParams.MATCH_PARENT
                ? MeasureSpec.makeMeasureSpec(Math.max(0, getMeasuredHeight() - getPaddingTop() - getPaddingBottom() - lp.topMargin - lp.bottomMargin), MeasureSpec.EXACTLY)
                : getChildMeasureSpec(heightMeasureSpec, getPaddingTop() + getPaddingBottom() + lp.topMargin + lp.bottomMargin, lp.height);
            c.measure(w, h);
        }
    }

    @Override protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
        int parentLeft = getPaddingLeft(), parentRight = right - left - getPaddingRight();
        int parentTop = getPaddingTop(), parentBottom = bottom - top - getPaddingBottom();
        for (int i = 0; i < getChildCount(); ++i) {
            View c = getChildAt(i);
            if (c.getVisibility() == GONE) continue;
            LayoutParams lp = (LayoutParams) c.getLayoutParams();
            int w = c.getMeasuredWidth(), h = c.getMeasuredHeight();
            int gravity = lp.gravity == -1 ? Gravity.TOP | Gravity.START : lp.gravity;
            int g = Gravity.getAbsoluteGravity(gravity, getLayoutDirection());
            int x, y;
            switch (g & Gravity.HORIZONTAL_GRAVITY_MASK) {
                case Gravity.CENTER_HORIZONTAL: x = parentLeft + (parentRight - parentLeft - w) / 2 + lp.leftMargin - lp.rightMargin; break;
                case Gravity.RIGHT: x = parentRight - w - lp.rightMargin; break;
                default: x = parentLeft + lp.leftMargin; break;
            }
            switch (gravity & Gravity.VERTICAL_GRAVITY_MASK) {
                case Gravity.CENTER_VERTICAL: y = parentTop + (parentBottom - parentTop - h) / 2 + lp.topMargin - lp.bottomMargin; break;
                case Gravity.BOTTOM: y = parentBottom - h - lp.bottomMargin; break;
                default: y = parentTop + lp.topMargin; break;
            }
            c.layout(x, y, x + w, y + h);
        }
    }

    @Override public CharSequence getAccessibilityClassName() { return FrameLayout.class.getName(); }
}
