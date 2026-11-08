// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable;

import android.content.res.Resources;
import android.util.StateSet;
import java.util.Arrays;

/** A drawable that shows the first child whose state set matches its state. */
public class StateListDrawable extends DrawableContainer {
    private final StateListState state;

    public StateListDrawable() { this(null, null); }
    private StateListDrawable(StateListState orig, Resources res) {
        state = new StateListState(orig, this, res);
        setConstantState(state);
        onStateChange(getState());
    }

    static final class StateListState extends DrawableContainerState {
        int[][] stateSets;

        StateListState(StateListState orig, StateListDrawable owner, Resources res) {
            super(orig, owner, res);
            stateSets = orig != null ? orig.stateSets.clone() : new int[getCapacity()][];
        }
        int add(int[] stateSet, Drawable drawable) {
            int pos = addChild(drawable);
            if (pos >= stateSets.length) stateSets = Arrays.copyOf(stateSets, getCapacity());
            stateSets[pos] = stateSet;
            return pos;
        }
        int indexOf(int[] stateSet) {
            for (int i = 0; i < getChildCount(); ++i) if (StateSet.stateSetMatches(stateSets[i], stateSet)) return i;
            return -1;
        }
        @Override public Drawable newDrawable() { return new StateListDrawable(this, null); }
        @Override public Drawable newDrawable(Resources res) { return new StateListDrawable(this, res); }
    }

    public void addState(int[] stateSet, Drawable drawable) {
        if (drawable == null) return;
        state.add(stateSet, drawable);
        onStateChange(getState());
    }
    @Override public boolean isStateful() { return true; }
    @Override protected boolean onStateChange(int[] stateSet) {
        boolean changed = super.onStateChange(stateSet);
        int idx = state.indexOf(stateSet);
        if (idx < 0) idx = state.indexOf(StateSet.WILD_CARD);
        return selectDrawable(idx) || changed;
    }
    /** android:drawable of an item */
    private static final int[] ITEM_ATTRS = {0x01010199};

    /** a selector's items: each the state set of its android:state_* attributes and its drawable */
    @Override public void inflate(Resources r, org.xmlpull.v1.XmlPullParser parser, android.util.AttributeSet attrs, Resources.Theme theme)
            throws org.xmlpull.v1.XmlPullParserException, java.io.IOException {
        super.inflate(r, parser, attrs, theme);
        int depth = parser.getDepth();
        while (DrawableXml.child(parser, depth)) {
            if (!parser.getName().equals("item")) {
                inflateChild(r, parser, attrs, theme);
                continue;
            }
            int[] states = DrawableXml.stateSet(attrs);
            int id = attrs.getAttributeResourceValue(DrawableXml.ANDROID, "id", 0);
            android.content.res.TypedArray a = DrawableXml.obtain(r, theme, attrs, ITEM_ATTRS);
            Drawable d;
            try {
                d = DrawableXml.itemDrawable(r, parser, attrs, theme, a, 0);
            } finally {
                a.recycle();
            }
            if (d != null) addItem(states, d, id);
        }
        onStateChange(getState());
    }
    /** an item of the list, with its android:id (an animated selector's keyframe) */
    void addItem(int[] states, Drawable d, int id) { addState(states, d); }
    /** a child element that is not an item: skipped (an animated selector's transition) */
    void inflateChild(Resources r, org.xmlpull.v1.XmlPullParser parser, android.util.AttributeSet attrs, Resources.Theme theme)
            throws org.xmlpull.v1.XmlPullParserException, java.io.IOException {
        DrawableXml.skip(parser);
    }

    public int getStateCount() { return state.getChildCount(); }
    public int[] getStateSet(int index) { return state.stateSets[index]; }
    public Drawable getStateDrawable(int index) { return state.getChild(index); }
    public int findStateDrawableIndex(int[] stateSet) { return state.indexOf(stateSet); }
}
