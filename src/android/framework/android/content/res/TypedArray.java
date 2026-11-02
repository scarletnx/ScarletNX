// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.res;

import android.graphics.drawable.Drawable;
import android.util.DisplayMetrics;
import android.util.TypedValue;

/** The values of attributes, STYLE_NUM_ENTRIES ints each, as the attribute retrieval natives write them. */
public class TypedArray implements AutoCloseable {
    static final int STYLE_NUM_ENTRIES = 7;
    static final int STYLE_TYPE = 0, STYLE_DATA = 1, STYLE_ASSET_COOKIE = 2, STYLE_RESOURCE_ID = 3,
        STYLE_CHANGING_CONFIGURATIONS = 4, STYLE_DENSITY = 5, STYLE_SOURCE_RESOURCE_ID = 6;

    private final Resources resources;
    final int[] data;
    final int[] indices;
    private final int length;
    Resources.Theme theme;
    XmlBlock.Parser xml;
    private boolean recycled;

    TypedArray(Resources resources, int length) {
        this.resources = resources;
        this.length = length;
        data = new int[length * STYLE_NUM_ENTRIES];
        indices = new int[length + 1];
    }

    static TypedArray obtain(Resources res, int length) {
        return new TypedArray(res, length);
    }

    private int at(int index) {
        if (recycled) throw new RuntimeException("Cannot make calls to a recycled instance!");
        return index * STYLE_NUM_ENTRIES;
    }

    public int length() { return length; }
    public int getIndexCount() { return indices[0]; }
    public int getIndex(int at) { return indices[1 + at]; }
    public Resources getResources() { return resources; }
    public int getType(int index) { return data[at(index) + STYLE_TYPE]; }
    public boolean hasValue(int index) { return data[at(index) + STYLE_TYPE] != TypedValue.TYPE_NULL; }
    public boolean hasValueOrEmpty(int index) {
        int i = at(index);
        return data[i + STYLE_TYPE] != TypedValue.TYPE_NULL || data[i + STYLE_DATA] == TypedValue.DATA_NULL_EMPTY;
    }
    public int getChangingConfigurations() {
        int c = 0;
        for (int i = 0; i < length; ++i) c |= data[i * STYLE_NUM_ENTRIES + STYLE_CHANGING_CONFIGURATIONS];
        return c;
    }
    public int getSourceResourceId(int index, int def) {
        int r = data[at(index) + STYLE_SOURCE_RESOURCE_ID];
        return r != 0 ? r : def;
    }

    private boolean value(int index, TypedValue out) {
        int i = at(index);
        int type = data[i + STYLE_TYPE];
        if (type == TypedValue.TYPE_NULL) return false;
        out.type = type;
        out.data = data[i + STYLE_DATA];
        out.assetCookie = data[i + STYLE_ASSET_COOKIE];
        out.resourceId = data[i + STYLE_RESOURCE_ID];
        out.changingConfigurations = data[i + STYLE_CHANGING_CONFIGURATIONS];
        out.density = data[i + STYLE_DENSITY];
        out.sourceResourceId = data[i + STYLE_SOURCE_RESOURCE_ID];
        out.string = type == TypedValue.TYPE_STRING ? string(out.assetCookie, out.data) : null;
        return true;
    }

    private CharSequence string(int cookie, int index) {
        if (cookie < 0) return xml != null ? xml.block().string(index) : null;
        return resources.getAssets().getPooledString(cookie, index);
    }

    public boolean getValue(int index, TypedValue out) { return value(index, out); }

    public TypedValue peekValue(int index) {
        TypedValue v = new TypedValue();
        return value(index, v) ? v : null;
    }

    public CharSequence getText(int index) {
        TypedValue v = peekValue(index);
        if (v == null) return null;
        if (v.type == TypedValue.TYPE_STRING) return v.string;
        return v.coerceToString();
    }

    public String getString(int index) {
        CharSequence s = getText(index);
        return s != null ? s.toString() : null;
    }

    public String getNonResourceString(int index) {
        int i = at(index);
        return data[i + STYLE_TYPE] == TypedValue.TYPE_STRING && data[i + STYLE_ASSET_COOKIE] < 0 ? getString(index) : null;
    }

    public String getNonConfigurationString(int index, int allowedChangingConfigs) { return getString(index); }

    public boolean getBoolean(int index, boolean def) {
        TypedValue v = peekValue(index);
        if (v == null) return def;
        if (v.type >= TypedValue.TYPE_FIRST_INT && v.type <= TypedValue.TYPE_LAST_INT) return v.data != 0;
        if (v.type == TypedValue.TYPE_STRING) return Boolean.parseBoolean(String.valueOf(v.string));
        return def;
    }

    public int getInt(int index, int def) {
        TypedValue v = peekValue(index);
        if (v == null) return def;
        if (v.type >= TypedValue.TYPE_FIRST_INT && v.type <= TypedValue.TYPE_LAST_INT) return v.data;
        if (v.type == TypedValue.TYPE_STRING) {
            try { return Integer.decode(String.valueOf(v.string)); } catch (NumberFormatException e) { return def; }
        }
        return def;
    }

    public int getInteger(int index, int def) { return getInt(index, def); }

    public float getFloat(int index, float def) {
        TypedValue v = peekValue(index);
        if (v == null) return def;
        if (v.type == TypedValue.TYPE_FLOAT) return v.getFloat();
        if (v.type >= TypedValue.TYPE_FIRST_INT && v.type <= TypedValue.TYPE_LAST_INT) return v.data;
        return def;
    }

    public int getColor(int index, int def) {
        TypedValue v = peekValue(index);
        if (v == null) return def;
        if (v.type >= TypedValue.TYPE_FIRST_INT && v.type <= TypedValue.TYPE_LAST_INT) return v.data;
        if (v.type == TypedValue.TYPE_STRING) return resources.loadColorStateList(v, v.resourceId, theme).getDefaultColor();
        if (v.type == TypedValue.TYPE_ATTRIBUTE)
            throw new UnsupportedOperationException("Failed to resolve attribute at index " + index + ": " + v);
        return def;
    }

    public ColorStateList getColorStateList(int index) {
        TypedValue v = peekValue(index);
        if (v == null) return null;
        return resources.loadColorStateList(v, v.resourceId, theme);
    }

    public float getDimension(int index, float def) {
        TypedValue v = peekValue(index);
        if (v == null) return def;
        if (v.type == TypedValue.TYPE_DIMENSION) return TypedValue.complexToDimension(v.data, metrics());
        return def;
    }

    public int getDimensionPixelOffset(int index, int def) {
        TypedValue v = peekValue(index);
        if (v == null) return def;
        if (v.type == TypedValue.TYPE_DIMENSION) return TypedValue.complexToDimensionPixelOffset(v.data, metrics());
        return def;
    }

    public int getDimensionPixelSize(int index, int def) {
        TypedValue v = peekValue(index);
        if (v == null) return def;
        if (v.type == TypedValue.TYPE_DIMENSION) return TypedValue.complexToDimensionPixelSize(v.data, metrics());
        return def;
    }

    public int getLayoutDimension(int index, String name) {
        TypedValue v = peekValue(index);
        if (v == null) throw new UnsupportedOperationException(getPositionDescription() + ": You must supply a " + name + " attribute.");
        return getLayoutDimension(index, 0);
    }

    public int getLayoutDimension(int index, int def) {
        TypedValue v = peekValue(index);
        if (v == null) return def;
        if (v.type >= TypedValue.TYPE_FIRST_INT && v.type <= TypedValue.TYPE_LAST_INT) return v.data;
        if (v.type == TypedValue.TYPE_DIMENSION) return TypedValue.complexToDimensionPixelSize(v.data, metrics());
        return def;
    }

    public float getFraction(int index, int base, int pbase, float def) {
        TypedValue v = peekValue(index);
        return v != null && v.type == TypedValue.TYPE_FRACTION ? TypedValue.complexToFraction(v.data, base, pbase) : def;
    }

    public int getResourceId(int index, int def) {
        int i = at(index);
        if (data[i + STYLE_TYPE] == TypedValue.TYPE_NULL) return def;
        int r = data[i + STYLE_RESOURCE_ID];
        if (r != 0) return r;
        return data[i + STYLE_TYPE] == TypedValue.TYPE_REFERENCE ? data[i + STYLE_DATA] : def;
    }

    public int getThemeAttributeId(int index, int def) {
        int i = at(index);
        return data[i + STYLE_TYPE] == TypedValue.TYPE_ATTRIBUTE ? data[i + STYLE_DATA] : def;
    }

    public Drawable getDrawable(int index) {
        int id = getResourceId(index, 0);
        if (id != 0) return resources.getDrawable(id, theme);
        TypedValue v = peekValue(index);
        if (v != null && v.type >= TypedValue.TYPE_FIRST_COLOR_INT && v.type <= TypedValue.TYPE_LAST_COLOR_INT)
            return new android.graphics.drawable.ColorDrawable(v.data);
        return null;
    }

    public android.graphics.Typeface getFont(int index) {
        return hasValue(index) ? android.graphics.Typeface.DEFAULT : null;
    }

    public CharSequence[] getTextArray(int index) {
        int id = getResourceId(index, 0);
        return id != 0 ? resources.getTextArray(id) : null;
    }

    private DisplayMetrics metrics() {
        return resources.getDisplayMetrics();
    }

    public String getPositionDescription() {
        return xml != null ? xml.getPositionDescription() : "<internal>";
    }

    public void recycle() { recycled = true; }
    @Override public void close() { recycle(); }

    @Override
    public String toString() {
        return "TypedArray{length=" + length + " values=" + indices[0] + "}";
    }
}
