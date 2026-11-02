// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.res;

import android.util.AttributeSet;
import android.util.StateSet;
import java.io.IOException;
import java.util.ArrayList;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;

/** Colors by view state: the first item whose states the view's states match. */
public class ColorStateList implements android.os.Parcelable {
    private static final int[][] EMPTY = {new int[0]};
    private int[][] stateSpecs;
    private int[] colors;
    private int defaultColor;

    public ColorStateList(int[][] states, int[] colors) {
        stateSpecs = states;
        this.colors = colors;
        defaultColor = colors.length > 0 ? colors[0] : 0;
        for (int i = 0; i < states.length; ++i)
            if (states[i].length == 0) { defaultColor = colors[i]; break; }
    }

    public static ColorStateList valueOf(int color) {
        return new ColorStateList(EMPTY, new int[] {color});
    }

    /** A selector of color items: android:color (a color or a reference), android:alpha, and the item's states. */
    public static ColorStateList createFromXml(Resources r, XmlPullParser parser, Resources.Theme theme)
        throws XmlPullParserException, IOException {
        int type;
        while ((type = parser.next()) != XmlPullParser.START_TAG && type != XmlPullParser.END_DOCUMENT) {}
        if (type != XmlPullParser.START_TAG) throw new XmlPullParserException("No start tag found");
        if (!parser.getName().equals("selector"))
            throw new XmlPullParserException(parser.getPositionDescription() + ": invalid color state list tag " + parser.getName());
        ArrayList<int[]> states = new ArrayList<>();
        ArrayList<Integer> colors = new ArrayList<>();
        int depth = parser.getDepth() + 1;
        while ((type = parser.next()) != XmlPullParser.END_DOCUMENT
            && (parser.getDepth() >= depth || type != XmlPullParser.END_TAG)) {
            if (type != XmlPullParser.START_TAG || parser.getDepth() > depth || !parser.getName().equals("item")) continue;
            AttributeSet attrs = (AttributeSet) parser;
            int color = 0xffff0000;
            float alpha = 1;
            ArrayList<Integer> spec = new ArrayList<>();
            for (int i = 0; i < attrs.getAttributeCount(); ++i) {
                int name = attrs.getAttributeNameResource(i);
                XmlBlock.Parser p = (XmlBlock.Parser) parser;
                if (name == 0x010101a5) { // android:color
                    color = resolveColor(r, theme, p.getAttributeDataType(i), p.getAttributeData(i));
                } else if (name == 0x0101031f) { // android:alpha
                    alpha = attrs.getAttributeFloatValue(i, 1);
                } else if (name != 0 && String.valueOf(attrs.getAttributeName(i)).startsWith("state_")) {
                    spec.add(attrs.getAttributeBooleanValue(i, false) ? name : -name);
                }
            }
            int a = Math.round(((color >>> 24) & 0xff) * alpha);
            colors.add((color & 0xffffff) | (Math.max(0, Math.min(255, a)) << 24));
            int[] s = new int[spec.size()];
            for (int i = 0; i < s.length; ++i) s[i] = spec.get(i);
            states.add(s);
        }
        int[] c = new int[colors.size()];
        for (int i = 0; i < c.length; ++i) c[i] = colors.get(i);
        return new ColorStateList(states.toArray(new int[0][]), c);
    }

    private static int resolveColor(Resources r, Resources.Theme theme, int type, int data) {
        android.util.TypedValue v = new android.util.TypedValue();
        if (type == android.util.TypedValue.TYPE_ATTRIBUTE && theme != null && theme.resolveAttribute(data, v, true)) {
            type = v.type;
            data = v.data;
        }
        if (type == android.util.TypedValue.TYPE_REFERENCE) return r.getColor(data, theme);
        return data;
    }

    public ColorStateList withAlpha(int alpha) {
        int[] c = colors.clone();
        for (int i = 0; i < c.length; ++i) c[i] = (c[i] & 0xffffff) | (alpha << 24);
        return new ColorStateList(stateSpecs, c);
    }

    public ColorStateList withLStar(float lStar) { return this; }

    public boolean isStateful() { return stateSpecs.length >= 1 && stateSpecs[0].length > 0; }
    public boolean isOpaque() {
        for (int c : colors) if ((c >>> 24) != 0xff) return false;
        return true;
    }
    public int getDefaultColor() { return defaultColor; }
    public int getChangingConfigurations() { return 0; }

    public int getColorForState(int[] stateSet, int defaultColor) {
        for (int i = 0; i < stateSpecs.length; ++i)
            if (StateSet.stateSetMatches(stateSpecs[i], stateSet)) return colors[i];
        return defaultColor;
    }

    public int[][] getStates() { return stateSpecs; }
    public int[] getColors() { return colors; }

    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeValue(this); }

    @Override
    public String toString() {
        return "ColorStateList{colors=" + java.util.Arrays.toString(colors) + "}";
    }
}
