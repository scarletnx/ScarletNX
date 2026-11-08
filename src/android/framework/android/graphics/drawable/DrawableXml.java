// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable;

import android.content.res.Resources;
import android.content.res.TypedArray;
import android.util.AttributeSet;
import java.io.IOException;
import java.util.Arrays;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;

/**
 * What the drawables' inflate methods share, as Android's do it: the attributes of an element (through the theme
 * when there is one, so ?attr references resolve), an item's drawable (its android:drawable, else its first child
 * element), its state set, and walking the child elements of the current one.
 */
final class DrawableXml {
    private DrawableXml() {}

    static final String ANDROID = "http://schemas.android.com/apk/res/android";

    static TypedArray obtain(Resources r, Resources.Theme theme, AttributeSet attrs, int[] ids) {
        return theme != null ? theme.obtainStyledAttributes(attrs, ids, 0, 0) : r.obtainAttributes(attrs, ids);
    }

    /** The depth of the current element's children: next() returns them while child(...) is true */
    static boolean child(XmlPullParser parser, int depth) throws XmlPullParserException, IOException {
        int type;
        while ((type = parser.next()) != XmlPullParser.END_DOCUMENT) {
            if (type == XmlPullParser.END_TAG && parser.getDepth() <= depth) return false;
            if (type == XmlPullParser.START_TAG && parser.getDepth() == depth + 1) return true;
        }
        return false;
    }

    /** The current element skipped to its end tag */
    static void skip(XmlPullParser parser) throws XmlPullParserException, IOException {
        int depth = parser.getDepth();
        while (child(parser, depth)) {}
    }

    /**
     * An item's drawable: its android:drawable attribute (index in the array a), else its first child element,
     * inflated. The parser ends at the item's end tag.
     */
    static Drawable itemDrawable(Resources r, XmlPullParser parser, AttributeSet attrs, Resources.Theme theme, TypedArray a,
                                 int index) throws XmlPullParserException, IOException {
        Drawable d = a.hasValue(index) ? drawableOf(r, a, index, theme) : null;
        int depth = parser.getDepth();
        while (child(parser, depth)) {
            if (d == null) d = Drawable.createFromXmlInner(r, parser, attrs, theme);
            else skip(parser);
        }
        return d;
    }

    /** The drawable that an attribute refers to, loaded with the theme */
    static Drawable drawableOf(Resources r, TypedArray a, int index, Resources.Theme theme) {
        int id = a.getResourceId(index, 0);
        if (id != 0) return r.getDrawable(id, theme);
        return a.getDrawable(index);
    }

    /**
     * The state set of an element: each android:state_* attribute, its id when true and its negation when false, as
     * Android's StateListDrawable reads it
     */
    static int[] stateSet(AttributeSet attrs) {
        int[] states = new int[attrs.getAttributeCount()];
        int n = 0;
        for (int i = 0; i < attrs.getAttributeCount(); ++i) {
            String name = attrs.getAttributeName(i);
            int res = attrs.getAttributeNameResource(i);
            if (res == 0 || name == null || !name.startsWith("state_")) continue;
            states[n++] = attrs.getAttributeBooleanValue(i, false) ? res : -res;
        }
        return Arrays.copyOf(states, n);
    }

    /** A dimension or a fraction of base (a fraction for a fraction value), as an inset may be either */
    static float dimensionOrFraction(TypedArray a, int index, float base, float def) {
        android.util.TypedValue v = a.peekValue(index);
        if (v == null) return def;
        if (v.type == android.util.TypedValue.TYPE_FRACTION) return v.getFraction(base, base);
        return a.getDimension(index, def);
    }
}
