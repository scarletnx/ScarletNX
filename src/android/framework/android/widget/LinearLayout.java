// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.widget;

import android.content.Context;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;

/** Children in one row or one column. A child with a weight gets that share of the space that remains. */
public class LinearLayout extends ViewGroup {
    public static final int HORIZONTAL = 0, VERTICAL = 1;
    public static final int SHOW_DIVIDER_NONE = 0, SHOW_DIVIDER_BEGINNING = 1, SHOW_DIVIDER_MIDDLE = 2, SHOW_DIVIDER_END = 4;
    private static final String NS = "http://schemas.android.com/apk/res/android";

    private int orientation = HORIZONTAL;
    private int gravity = Gravity.START | Gravity.TOP;
    private float weightSum = -1;
    private boolean baselineAligned = true, measureWithLargestChild;
    private int baselineAlignedChildIndex = -1, showDividers, dividerPadding;
    private Drawable divider;
    private int totalLength;

    public static class LayoutParams extends MarginLayoutParams {
        public float weight;
        public int gravity = -1;

        public LayoutParams(Context c, AttributeSet attrs) {
            super(c, attrs);
            if (attrs != null) {
                weight = attrs.getAttributeFloatValue(NS, "layout_weight", 0);
                gravity = attrs.getAttributeIntValue(NS, "layout_gravity", -1);
            }
        }
        public LayoutParams(int width, int height) { super(width, height); }
        public LayoutParams(int width, int height, float weight) { super(width, height); this.weight = weight; }
        public LayoutParams(ViewGroup.LayoutParams p) { super(p); }
        public LayoutParams(MarginLayoutParams source) { super(source); }
        public LayoutParams(LayoutParams source) { super(source); weight = source.weight; gravity = source.gravity; }
    }

    public LinearLayout(Context context) { super(context); }
    public LinearLayout(Context context, AttributeSet attrs) { super(context, attrs); init(attrs); }
    public LinearLayout(Context context, AttributeSet attrs, int defStyleAttr) { super(context, attrs, defStyleAttr); init(attrs); }
    public LinearLayout(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) { super(context, attrs, defStyleAttr, defStyleRes); init(attrs); }
    private void init(AttributeSet attrs) {
        if (attrs == null) return;
        orientation = attrs.getAttributeIntValue(NS, "orientation", orientation);
        int g = attrs.getAttributeIntValue(NS, "gravity", -1);
        if (g != -1) setGravity(g);
        weightSum = attrs.getAttributeFloatValue(NS, "weightSum", -1);
        baselineAligned = attrs.getAttributeBooleanValue(NS, "baselineAligned", true);
    }

    public void setOrientation(int orientation) { if (this.orientation != orientation) { this.orientation = orientation; requestLayout(); } }
    public int getOrientation() { return orientation; }
    public void setGravity(int gravity) {
        if ((gravity & Gravity.RELATIVE_HORIZONTAL_GRAVITY_MASK) == 0) gravity |= Gravity.START;
        if ((gravity & Gravity.VERTICAL_GRAVITY_MASK) == 0) gravity |= Gravity.TOP;
        if (this.gravity != gravity) { this.gravity = gravity; requestLayout(); }
    }
    public int getGravity() { return gravity; }
    public void setHorizontalGravity(int horizontalGravity) { setGravity((gravity & ~Gravity.RELATIVE_HORIZONTAL_GRAVITY_MASK) | (horizontalGravity & Gravity.RELATIVE_HORIZONTAL_GRAVITY_MASK)); }
    public void setVerticalGravity(int verticalGravity) { setGravity((gravity & ~Gravity.VERTICAL_GRAVITY_MASK) | (verticalGravity & Gravity.VERTICAL_GRAVITY_MASK)); }
    public void setWeightSum(float weightSum) { this.weightSum = Math.max(0, weightSum); }
    public float getWeightSum() { return weightSum; }
    public void setBaselineAligned(boolean baselineAligned) { this.baselineAligned = baselineAligned; }
    public boolean isBaselineAligned() { return baselineAligned; }
    public void setBaselineAlignedChildIndex(int i) { baselineAlignedChildIndex = i; }
    public int getBaselineAlignedChildIndex() { return baselineAlignedChildIndex; }
    public void setMeasureWithLargestChildEnabled(boolean enabled) { measureWithLargestChild = enabled; }
    public boolean isMeasureWithLargestChildEnabled() { return measureWithLargestChild; }
    public void setDividerDrawable(Drawable divider) { this.divider = divider; requestLayout(); }
    public Drawable getDividerDrawable() { return divider; }
    public void setShowDividers(int showDividers) { this.showDividers = showDividers; requestLayout(); }
    public int getShowDividers() { return showDividers; }
    public void setDividerPadding(int padding) { dividerPadding = padding; }
    public int getDividerPadding() { return dividerPadding; }
    // View declares this in Android; until the View here has it, LinearLayout declares it.
    protected void onFinishInflate() {}

    @Override protected LayoutParams generateDefaultLayoutParams() {
        return orientation == HORIZONTAL ? new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT) : new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT);
    }
    @Override public LayoutParams generateLayoutParams(AttributeSet attrs) { return new LayoutParams(getContext(), attrs); }
    @Override protected ViewGroup.LayoutParams generateLayoutParams(ViewGroup.LayoutParams lp) {
        if (lp instanceof LayoutParams l) return new LayoutParams(l);
        if (lp instanceof MarginLayoutParams m) return new LayoutParams(m);
        return new LayoutParams(lp);
    }
    @Override protected boolean checkLayoutParams(ViewGroup.LayoutParams p) { return p instanceof LayoutParams; }

    // ---- measure, along the main axis (the orientation) and the cross axis ----
    private boolean vertical() { return orientation == VERTICAL; }
    private int main(View c) { return vertical() ? c.getMeasuredHeight() : c.getMeasuredWidth(); }
    private int cross(View c) { return vertical() ? c.getMeasuredWidth() : c.getMeasuredHeight(); }
    private int mainMargins(LayoutParams lp) { return vertical() ? lp.topMargin + lp.bottomMargin : lp.leftMargin + lp.rightMargin; }
    private int crossMargins(LayoutParams lp) { return vertical() ? lp.leftMargin + lp.rightMargin : lp.topMargin + lp.bottomMargin; }
    private int mainPadding() { return vertical() ? getPaddingTop() + getPaddingBottom() : getPaddingLeft() + getPaddingRight(); }
    private int crossPadding() { return vertical() ? getPaddingLeft() + getPaddingRight() : getPaddingTop() + getPaddingBottom(); }
    private int mainDim(LayoutParams lp) { return vertical() ? lp.height : lp.width; }
    private int crossDim(LayoutParams lp) { return vertical() ? lp.width : lp.height; }
    private void measure(View c, int mainSpec, int mainUsed, int crossSpec) {
        if (vertical()) measureChildWithMargins(c, crossSpec, 0, mainSpec, mainUsed);
        else measureChildWithMargins(c, mainSpec, mainUsed, crossSpec, 0);
    }
    private void measureExact(View c, int mainSize, int crossSpec) {
        LayoutParams lp = (LayoutParams) c.getLayoutParams();
        int m = MeasureSpec.makeMeasureSpec(Math.max(0, mainSize), MeasureSpec.EXACTLY);
        int x = getChildMeasureSpec(crossSpec, crossPadding() + crossMargins(lp), crossDim(lp));
        if (vertical()) c.measure(x, m); else c.measure(m, x);
    }

    @Override protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int mainSpec = vertical() ? heightMeasureSpec : widthMeasureSpec, crossSpec = vertical() ? widthMeasureSpec : heightMeasureSpec;
        boolean mainExact = MeasureSpec.getMode(mainSpec) == MeasureSpec.EXACTLY;
        int count = getChildCount(), maxCross = 0, state = 0;
        float totalWeight = 0;
        boolean matchCross = false;
        totalLength = 0;
        for (int i = 0; i < count; ++i) {
            View c = getChildAt(i);
            if (c.getVisibility() == GONE) continue;
            LayoutParams lp = (LayoutParams) c.getLayoutParams();
            totalWeight += lp.weight;
            if (mainExact && mainDim(lp) == 0 && lp.weight > 0) {
                totalLength += mainMargins(lp); // measured below, with its share
            } else {
                measure(c, mainSpec, totalWeight == 0 ? totalLength : 0, crossSpec);
                totalLength += main(c) + mainMargins(lp);
                state = combineMeasuredStates(state, c.getMeasuredState());
            }
            if (crossDim(lp) == LayoutParams.MATCH_PARENT) matchCross = true;
            maxCross = Math.max(maxCross, cross(c) + crossMargins(lp));
        }
        totalLength += mainPadding();
        int minMain = vertical() ? getSuggestedMinimumHeight() : getSuggestedMinimumWidth();
        int mainSize = resolveSizeAndState(Math.max(totalLength, minMain), mainSpec, 0) & MEASURED_SIZE_MASK;
        int delta = mainSize - totalLength;
        if (totalWeight > 0 && delta != 0) {
            float sum = weightSum > 0 ? weightSum : totalWeight;
            totalLength = 0;
            maxCross = 0;
            for (int i = 0; i < count; ++i) {
                View c = getChildAt(i);
                if (c.getVisibility() == GONE) continue;
                LayoutParams lp = (LayoutParams) c.getLayoutParams();
                if (lp.weight > 0) {
                    int share = (int) (lp.weight * delta / sum);
                    sum -= lp.weight;
                    delta -= share;
                    boolean fresh = mainExact && mainDim(lp) == 0;
                    measureExact(c, fresh ? share : main(c) + share, crossSpec);
                    state = combineMeasuredStates(state, c.getMeasuredState());
                }
                totalLength += main(c) + mainMargins(lp);
                maxCross = Math.max(maxCross, cross(c) + crossMargins(lp));
            }
            totalLength += mainPadding();
        }
        maxCross += crossPadding();
        int minCross = vertical() ? getSuggestedMinimumWidth() : getSuggestedMinimumHeight();
        int crossSize = resolveSizeAndState(Math.max(maxCross, minCross), crossSpec, 0) & MEASURED_SIZE_MASK;
        if (vertical()) setMeasuredDimension(resolveSizeAndState(crossSize, crossSpec, state), resolveSizeAndState(mainSize, mainSpec, state << MEASURED_HEIGHT_STATE_SHIFT));
        else setMeasuredDimension(resolveSizeAndState(mainSize, mainSpec, state), resolveSizeAndState(crossSize, crossSpec, state << MEASURED_HEIGHT_STATE_SHIFT));
        if (matchCross && MeasureSpec.getMode(crossSpec) != MeasureSpec.EXACTLY) {
            // children that match the cross size get the size that the layout now has
            int exact = MeasureSpec.makeMeasureSpec(vertical() ? getMeasuredWidth() : getMeasuredHeight(), MeasureSpec.EXACTLY);
            for (int i = 0; i < count; ++i) {
                View c = getChildAt(i);
                if (c.getVisibility() == GONE || crossDim((LayoutParams) c.getLayoutParams()) != LayoutParams.MATCH_PARENT) continue;
                measureExact(c, main(c), exact);
            }
        }
    }

    @Override protected void onLayout(boolean changed, int l, int t, int r, int b) {
        boolean v = vertical();
        int absGravity = Gravity.getAbsoluteGravity(gravity, getLayoutDirection());
        int mainGravity = v ? gravity & Gravity.VERTICAL_GRAVITY_MASK : absGravity & Gravity.HORIZONTAL_GRAVITY_MASK;
        int space = v ? b - t : r - l;
        int pos = v ? getPaddingTop() : getPaddingLeft();
        if (mainGravity == (v ? Gravity.BOTTOM : Gravity.RIGHT)) pos += space - totalLength;
        else if (mainGravity == (v ? Gravity.CENTER_VERTICAL : Gravity.CENTER_HORIZONTAL)) pos += (space - totalLength) / 2;
        int crossStart = v ? getPaddingLeft() : getPaddingTop(), crossEnd = (v ? r - l - getPaddingRight() : b - t - getPaddingBottom());
        for (int i = 0; i < getChildCount(); ++i) {
            View c = getChildAt(i);
            if (c.getVisibility() == GONE) continue;
            LayoutParams lp = (LayoutParams) c.getLayoutParams();
            int w = c.getMeasuredWidth(), h = c.getMeasuredHeight();
            int g = lp.gravity < 0 ? gravity : lp.gravity;
            int x, y;
            if (v) {
                switch (Gravity.getAbsoluteGravity(g, getLayoutDirection()) & Gravity.HORIZONTAL_GRAVITY_MASK) {
                    case Gravity.CENTER_HORIZONTAL: x = crossStart + (crossEnd - crossStart - w) / 2 + lp.leftMargin - lp.rightMargin; break;
                    case Gravity.RIGHT: x = crossEnd - w - lp.rightMargin; break;
                    default: x = crossStart + lp.leftMargin; break;
                }
                y = pos + lp.topMargin;
                pos = y + h + lp.bottomMargin;
            } else {
                switch (g & Gravity.VERTICAL_GRAVITY_MASK) {
                    case Gravity.CENTER_VERTICAL: y = crossStart + (crossEnd - crossStart - h) / 2 + lp.topMargin - lp.bottomMargin; break;
                    case Gravity.BOTTOM: y = crossEnd - h - lp.bottomMargin; break;
                    default: y = crossStart + lp.topMargin; break;
                }
                x = pos + lp.leftMargin;
                pos = x + w + lp.rightMargin;
            }
            c.layout(x, y, x + w, y + h);
        }
    }
    @Override public boolean shouldDelayChildPressedState() { return false; }
    @Override public CharSequence getAccessibilityClassName() { return LinearLayout.class.getName(); }
}
