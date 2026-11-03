// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.util.AttributeSet;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import java.util.ArrayList;

/** Children placed by rules: against the edges of the layout, or against siblings by their ids. */
public class RelativeLayout extends ViewGroup {
    public static final int TRUE = -1;
    public static final int LEFT_OF = 0, RIGHT_OF = 1, ABOVE = 2, BELOW = 3, ALIGN_BASELINE = 4, ALIGN_LEFT = 5, ALIGN_TOP = 6,
        ALIGN_RIGHT = 7, ALIGN_BOTTOM = 8, ALIGN_PARENT_LEFT = 9, ALIGN_PARENT_TOP = 10, ALIGN_PARENT_RIGHT = 11,
        ALIGN_PARENT_BOTTOM = 12, CENTER_IN_PARENT = 13, CENTER_HORIZONTAL = 14, CENTER_VERTICAL = 15, START_OF = 16,
        END_OF = 17, ALIGN_START = 18, ALIGN_END = 19, ALIGN_PARENT_START = 20, ALIGN_PARENT_END = 21;
    private static final int VERB_COUNT = 22, UNSET = Integer.MIN_VALUE;
    private static final String NS = "http://schemas.android.com/apk/res/android";
    // the XML attribute of each rule, by verb
    private static final String[] RULE_ATTRS = {"layout_toLeftOf", "layout_toRightOf", "layout_above", "layout_below",
        "layout_alignBaseline", "layout_alignLeft", "layout_alignTop", "layout_alignRight", "layout_alignBottom",
        "layout_alignParentLeft", "layout_alignParentTop", "layout_alignParentRight", "layout_alignParentBottom",
        "layout_centerInParent", "layout_centerHorizontal", "layout_centerVertical", "layout_toStartOf", "layout_toEndOf",
        "layout_alignStart", "layout_alignEnd", "layout_alignParentStart", "layout_alignParentEnd"};

    private int gravity = Gravity.START | Gravity.TOP;
    private int ignoreGravity = View.NO_ID;

    public static class LayoutParams extends MarginLayoutParams {
        private final int[] rules = new int[VERB_COUNT];
        public boolean alignWithParent;
        int left, top, right, bottom;

        public LayoutParams(Context c, AttributeSet attrs) {
            super(c, attrs);
            if (attrs == null) return;
            for (int verb = 0; verb < VERB_COUNT; ++verb) {
                boolean flag = verb >= ALIGN_PARENT_LEFT && verb <= CENTER_VERTICAL || verb >= ALIGN_PARENT_START;
                if (flag) rules[verb] = attrs.getAttributeBooleanValue(NS, RULE_ATTRS[verb], false) ? TRUE : 0;
                else rules[verb] = attrs.getAttributeResourceValue(NS, RULE_ATTRS[verb], 0);
            }
            alignWithParent = attrs.getAttributeBooleanValue(NS, "layout_alignWithParentIfMissing", false);
        }
        public LayoutParams(int w, int h) { super(w, h); }
        public LayoutParams(ViewGroup.LayoutParams source) { super(source); }
        public LayoutParams(MarginLayoutParams source) { super(source); }
        public LayoutParams(LayoutParams source) {
            super(source);
            alignWithParent = source.alignWithParent;
            System.arraycopy(source.rules, 0, rules, 0, VERB_COUNT);
        }

        public void addRule(int verb) { addRule(verb, TRUE); }
        public void addRule(int verb, int subject) { rules[verb] = subject; }
        public void removeRule(int verb) { rules[verb] = 0; }
        public int getRule(int verb) { return rules[verb]; }
        public int[] getRules() { return rules; }
        public int[] getRules(int layoutDirection) { return rules; }
        /** The rule of a verb, with its relative form (start, end) read for a left-to-right layout. */
        int rule(int verb) {
            int relative = switch (verb) {
                case LEFT_OF -> START_OF; case RIGHT_OF -> END_OF; case ALIGN_LEFT -> ALIGN_START; case ALIGN_RIGHT -> ALIGN_END;
                case ALIGN_PARENT_LEFT -> ALIGN_PARENT_START; case ALIGN_PARENT_RIGHT -> ALIGN_PARENT_END; default -> -1;
            };
            return relative >= 0 && rules[relative] != 0 ? rules[relative] : rules[verb];
        }
    }

    public RelativeLayout(Context context) { super(context); }
    public RelativeLayout(Context context, AttributeSet attrs) { super(context, attrs); }
    public RelativeLayout(Context context, AttributeSet attrs, int defStyleAttr) { super(context, attrs, defStyleAttr); }
    public RelativeLayout(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) { super(context, attrs, defStyleAttr, defStyleRes); }

    public void setGravity(int gravity) { if (this.gravity != gravity) { this.gravity = gravity; requestLayout(); } }
    public int getGravity() { return gravity; }
    public void setHorizontalGravity(int horizontalGravity) { setGravity((gravity & ~Gravity.RELATIVE_HORIZONTAL_GRAVITY_MASK) | (horizontalGravity & Gravity.RELATIVE_HORIZONTAL_GRAVITY_MASK)); }
    public void setVerticalGravity(int verticalGravity) { setGravity((gravity & ~Gravity.VERTICAL_GRAVITY_MASK) | (verticalGravity & Gravity.VERTICAL_GRAVITY_MASK)); }
    public void setIgnoreGravity(int viewId) { ignoreGravity = viewId; }

    @Override protected ViewGroup.LayoutParams generateDefaultLayoutParams() { return new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT); }
    @Override public LayoutParams generateLayoutParams(AttributeSet attrs) { return new LayoutParams(getContext(), attrs); }
    @Override protected ViewGroup.LayoutParams generateLayoutParams(ViewGroup.LayoutParams lp) {
        if (lp instanceof LayoutParams l) return new LayoutParams(l);
        if (lp instanceof MarginLayoutParams m) return new LayoutParams(m);
        return new LayoutParams(lp);
    }
    @Override protected boolean checkLayoutParams(ViewGroup.LayoutParams p) { return p instanceof LayoutParams; }
    @Override public boolean shouldDelayChildPressedState() { return false; }

    /** The children in an order where each comes after the siblings that its rules name. */
    private ArrayList<View> sorted() {
        ArrayList<View> pending = new ArrayList<>(), out = new ArrayList<>();
        for (int i = 0; i < getChildCount(); ++i) if (getChildAt(i).getVisibility() != GONE) pending.add(getChildAt(i));
        while (!pending.isEmpty()) {
            boolean progress = false;
            for (int i = 0; i < pending.size(); ++i) {
                LayoutParams lp = (LayoutParams) pending.get(i).getLayoutParams();
                boolean ready = true;
                for (int verb : new int[] {LEFT_OF, RIGHT_OF, ABOVE, BELOW, ALIGN_BASELINE, ALIGN_LEFT, ALIGN_TOP, ALIGN_RIGHT, ALIGN_BOTTOM}) {
                    int id = lp.rule(verb);
                    if (id <= 0) continue;
                    for (View p : pending) if (p.getId() == id && p != pending.get(i)) { ready = false; break; }
                    if (!ready) break;
                }
                if (ready) { out.add(pending.remove(i--)); progress = true; }
            }
            if (!progress) { out.addAll(pending); break; } // a cycle: the rest in child order
        }
        return out;
    }
    /** The sibling that a rule names, if it is placed; null when it is missing or gone. */
    private LayoutParams anchor(LayoutParams lp, int verb) {
        int id = lp.rule(verb);
        if (id <= 0) return null;
        for (int i = 0; i < getChildCount(); ++i) {
            View c = getChildAt(i);
            if (c.getId() == id && c.getVisibility() != GONE) return (LayoutParams) c.getLayoutParams();
        }
        return null;
    }
    /** The measure spec of a child between two edges; an edge that no rule sets is the edge of the layout. */
    private static int spec(int start, int end, int dim, int defStart, int defEnd, boolean known) {
        if (start != UNSET && end != UNSET) return MeasureSpec.makeMeasureSpec(Math.max(0, end - start), MeasureSpec.EXACTLY);
        if (dim >= 0) return MeasureSpec.makeMeasureSpec(dim, MeasureSpec.EXACTLY);
        if (!known && end == UNSET) return MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED);
        int avail = Math.max(0, (end != UNSET ? end : defEnd) - (start != UNSET ? start : defStart));
        return MeasureSpec.makeMeasureSpec(avail, dim == LayoutParams.MATCH_PARENT ? MeasureSpec.EXACTLY : MeasureSpec.AT_MOST);
    }

    /** Measures and places each child in a layout of the given size; a negative size is not known yet. */
    private void place(int myW, int myH) {
        int pl = getPaddingLeft(), pt = getPaddingTop(), pr = getPaddingRight(), pb = getPaddingBottom();
        for (View c : sorted()) {
            LayoutParams lp = (LayoutParams) c.getLayoutParams();
            int l = UNSET, r = UNSET, t = UNSET, b = UNSET;
            LayoutParams a;
            if ((a = anchor(lp, LEFT_OF)) != null) r = a.left - a.leftMargin - lp.rightMargin;
            else if (lp.alignWithParent && lp.rule(LEFT_OF) != 0 && myW >= 0) r = myW - pr - lp.rightMargin;
            if ((a = anchor(lp, RIGHT_OF)) != null) l = a.right + a.rightMargin + lp.leftMargin;
            else if (lp.alignWithParent && lp.rule(RIGHT_OF) != 0) l = pl + lp.leftMargin;
            if ((a = anchor(lp, ALIGN_LEFT)) != null) l = a.left + lp.leftMargin;
            if ((a = anchor(lp, ALIGN_RIGHT)) != null) r = a.right - lp.rightMargin;
            if (lp.rule(ALIGN_PARENT_LEFT) != 0) l = pl + lp.leftMargin;
            if (lp.rule(ALIGN_PARENT_RIGHT) != 0 && myW >= 0) r = myW - pr - lp.rightMargin;
            if ((a = anchor(lp, ABOVE)) != null) b = a.top - a.topMargin - lp.bottomMargin;
            else if (lp.alignWithParent && lp.rule(ABOVE) != 0 && myH >= 0) b = myH - pb - lp.bottomMargin;
            if ((a = anchor(lp, BELOW)) != null) t = a.bottom + a.bottomMargin + lp.topMargin;
            else if (lp.alignWithParent && lp.rule(BELOW) != 0) t = pt + lp.topMargin;
            if ((a = anchor(lp, ALIGN_TOP)) != null || (a = anchor(lp, ALIGN_BASELINE)) != null) t = a.top + lp.topMargin;
            if ((a = anchor(lp, ALIGN_BOTTOM)) != null) b = a.bottom - lp.bottomMargin;
            if (lp.rule(ALIGN_PARENT_TOP) != 0) t = pt + lp.topMargin;
            if (lp.rule(ALIGN_PARENT_BOTTOM) != 0 && myH >= 0) b = myH - pb - lp.bottomMargin;
            int ws = spec(l, r, lp.width, pl + lp.leftMargin, myW - pr - lp.rightMargin, myW >= 0);
            int hs = spec(t, b, lp.height, pt + lp.topMargin, myH - pb - lp.bottomMargin, myH >= 0);
            c.measure(ws, hs);
            int w = c.getMeasuredWidth(), h = c.getMeasuredHeight();
            boolean centerH = lp.rule(CENTER_IN_PARENT) != 0 || lp.rule(CENTER_HORIZONTAL) != 0;
            boolean centerV = lp.rule(CENTER_IN_PARENT) != 0 || lp.rule(CENTER_VERTICAL) != 0;
            if (l == UNSET && r == UNSET) {
                if (centerH && myW >= 0) l = (myW - w) / 2;
                else if (c.getId() != ignoreGravity && myW >= 0 && (Gravity.getAbsoluteGravity(gravity, getLayoutDirection()) & Gravity.HORIZONTAL_GRAVITY_MASK) == Gravity.RIGHT) l = myW - pr - lp.rightMargin - w;
                else l = pl + lp.leftMargin;
            }
            if (l == UNSET) l = r - w; else r = l + w;
            if (t == UNSET && b == UNSET) {
                if (centerV && myH >= 0) t = (myH - h) / 2;
                else if (c.getId() != ignoreGravity && myH >= 0 && (gravity & Gravity.VERTICAL_GRAVITY_MASK) == Gravity.BOTTOM) t = myH - pb - lp.bottomMargin - h;
                else t = pt + lp.topMargin;
            }
            if (t == UNSET) t = b - h; else b = t + h;
            lp.left = l; lp.right = r; lp.top = t; lp.bottom = b;
        }
    }

    @Override protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int wmode = MeasureSpec.getMode(widthMeasureSpec), hmode = MeasureSpec.getMode(heightMeasureSpec);
        int wsize = MeasureSpec.getSize(widthMeasureSpec), hsize = MeasureSpec.getSize(heightMeasureSpec);
        int myW = wmode == MeasureSpec.UNSPECIFIED ? -1 : wsize, myH = hmode == MeasureSpec.UNSPECIFIED ? -1 : hsize;
        place(myW, myH);
        int width = myW, height = myH;
        if (wmode != MeasureSpec.EXACTLY || hmode != MeasureSpec.EXACTLY) {
            int maxR = getPaddingLeft(), maxB = getPaddingTop();
            for (int i = 0; i < getChildCount(); ++i) {
                View c = getChildAt(i);
                if (c.getVisibility() == GONE) continue;
                LayoutParams lp = (LayoutParams) c.getLayoutParams();
                maxR = Math.max(maxR, lp.right + lp.rightMargin);
                maxB = Math.max(maxB, lp.bottom + lp.bottomMargin);
            }
            if (wmode != MeasureSpec.EXACTLY) {
                width = Math.max(maxR + getPaddingRight(), getSuggestedMinimumWidth());
                if (wmode == MeasureSpec.AT_MOST) width = Math.min(width, wsize);
            }
            if (hmode != MeasureSpec.EXACTLY) {
                height = Math.max(maxB + getPaddingBottom(), getSuggestedMinimumHeight());
                if (hmode == MeasureSpec.AT_MOST) height = Math.min(height, hsize);
            }
            if (width != myW || height != myH) place(width, height);
        }
        setMeasuredDimension(width, height);
    }
    @Override protected void onLayout(boolean changed, int l, int t, int r, int b) {
        for (int i = 0; i < getChildCount(); ++i) {
            View c = getChildAt(i);
            if (c.getVisibility() == GONE) continue;
            LayoutParams lp = (LayoutParams) c.getLayoutParams();
            c.layout(lp.left, lp.top, lp.right, lp.bottom);
        }
    }
    @Override public CharSequence getAccessibilityClassName() { return RelativeLayout.class.getName(); }
}
