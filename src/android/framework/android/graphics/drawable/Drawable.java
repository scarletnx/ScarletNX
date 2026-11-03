// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable;

import android.content.res.Resources;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Insets;
import android.graphics.Outline;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.util.AttributeSet;
import android.util.TypedValue;
import java.io.IOException;
import java.io.InputStream;
import java.lang.ref.WeakReference;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;

/** Something that draws itself into bounds: the base of the framework's drawables. */
public abstract class Drawable {
    private static final Rect ZERO_BOUNDS = new Rect();
    private Rect bounds = ZERO_BOUNDS;
    private int[] stateSet = android.util.StateSet.WILD_CARD;
    private int level;
    private int changingConfigurations;
    private WeakReference<Callback> callback;
    private boolean visible = true;
    private int layoutDirection;

    public interface Callback {
        void invalidateDrawable(Drawable who);

        void scheduleDrawable(Drawable who, Runnable what, long when);

        void unscheduleDrawable(Drawable who, Runnable what);
    }

    public abstract static class ConstantState {
        public abstract Drawable newDrawable();

        public Drawable newDrawable(Resources res) { return newDrawable(); }

        public Drawable newDrawable(Resources res, Resources.Theme theme) { return newDrawable(res); }

        public abstract int getChangingConfigurations();

        public boolean canApplyTheme() { return false; }
    }

    /** The drawable of a resource: a color, or for now nothing else (snx: drawables are loaded as apps reach them). */
    public static Drawable load(Resources res, int id, Resources.Theme theme) {
        TypedValue v = new TypedValue();
        res.getValue(id, v, true);
        if (v.type >= TypedValue.TYPE_FIRST_COLOR_INT && v.type <= TypedValue.TYPE_LAST_COLOR_INT)
            return new ColorDrawable(v.data);
        throw new UnsupportedOperationException("snx: the drawable " + v.string + " (resource #0x" + Integer.toHexString(id) + ")");
    }

    /** A drawable of an encoded image: a BitmapDrawable, or a NinePatchDrawable when the image has a nine-patch chunk. */
    public static Drawable createFromResourceStream(Resources res, TypedValue value, InputStream is, String srcName) {
        return createFromResourceStream(res, value, is, srcName, null);
    }
    public static Drawable createFromResourceStream(Resources res, TypedValue value, InputStream is, String srcName,
                                                    android.graphics.BitmapFactory.Options opts) {
        if (is == null) return null;
        Rect pad = new Rect();
        if (opts == null) opts = new android.graphics.BitmapFactory.Options();
        android.graphics.Bitmap bm = android.graphics.BitmapFactory.decodeResourceStream(res, value, is, pad, opts);
        if (bm == null) return null;
        byte[] chunk = bm.getNinePatchChunk();
        if (chunk != null && android.graphics.NinePatch.isNinePatchChunk(chunk)) return new NinePatchDrawable(res, bm, chunk, pad, srcName);
        return new BitmapDrawable(res, bm);
    }
    public static Drawable createFromStream(InputStream is, String srcName) { return createFromResourceStream(null, null, is, srcName); }
    public static Drawable createFromPath(String pathName) {
        if (pathName == null) return null;
        try (InputStream is = new java.io.FileInputStream(pathName)) {
            return createFromResourceStream(null, null, is, pathName);
        } catch (IOException e) {
            return null;
        }
    }
    /** The drawable of an XML element that the parser is on: an empty drawable of the tag's class, then inflated. */
    public static Drawable createFromXmlInner(Resources r, XmlPullParser parser, AttributeSet attrs, Resources.Theme theme)
            throws XmlPullParserException, IOException {
        Drawable d;
        switch (parser.getName()) {
            case "selector": d = new StateListDrawable(); break;
            case "animation-list": d = new AnimationDrawable(); break;
            case "layer-list": d = new LayerDrawable(); break;
            case "shape": d = new GradientDrawable(); break;
            case "vector": d = new VectorDrawable(); break;
            case "animated-vector": d = new AnimatedVectorDrawable(); break;
            case "color": d = new ColorDrawable(); break;
            default: throw new XmlPullParserException(parser.getPositionDescription() + ": invalid drawable tag " + parser.getName());
        }
        d.inflate(r, parser, attrs, theme);
        return d;
    }
    public static Drawable createFromXmlInner(Resources r, XmlPullParser parser, AttributeSet attrs) throws XmlPullParserException, IOException {
        return createFromXmlInner(r, parser, attrs, null);
    }
    public void inflate(Resources r, XmlPullParser parser, AttributeSet attrs) throws XmlPullParserException, IOException {
        inflate(r, parser, attrs, null);
    }
    /** The base drawable reads only android:visible. */
    public void inflate(Resources r, XmlPullParser parser, AttributeSet attrs, Resources.Theme theme) throws XmlPullParserException, IOException {
        visible = attrs.getAttributeBooleanValue("http://schemas.android.com/apk/res/android", "visible", visible);
    }

    public abstract void draw(Canvas canvas);

    public void setBounds(int left, int top, int right, int bottom) {
        Rect old = bounds;
        if (old == ZERO_BOUNDS) old = bounds = new Rect();
        if (old.left != left || old.top != top || old.right != right || old.bottom != bottom) {
            if (!old.isEmpty()) invalidateSelf();
            bounds.set(left, top, right, bottom);
            onBoundsChange(bounds);
        }
    }

    public void setBounds(Rect b) { setBounds(b.left, b.top, b.right, b.bottom); }
    public final void copyBounds(Rect bounds) { bounds.set(this.bounds); }
    public final Rect copyBounds() { return new Rect(bounds); }
    public final Rect getBounds() {
        if (bounds == ZERO_BOUNDS) bounds = new Rect();
        return bounds;
    }
    public Rect getDirtyBounds() { return getBounds(); }
    public void setChangingConfigurations(int configs) { changingConfigurations = configs; }
    public int getChangingConfigurations() { return changingConfigurations; }
    @Deprecated public void setDither(boolean dither) {}
    public void setFilterBitmap(boolean filter) {}
    public boolean isFilterBitmap() { return false; }
    public final void setCallback(Callback cb) { callback = cb != null ? new WeakReference<>(cb) : null; }
    public Callback getCallback() { return callback != null ? callback.get() : null; }
    public void invalidateSelf() { Callback cb = getCallback(); if (cb != null) cb.invalidateDrawable(this); }
    public void scheduleSelf(Runnable what, long when) { Callback cb = getCallback(); if (cb != null) cb.scheduleDrawable(this, what, when); }
    public void unscheduleSelf(Runnable what) { Callback cb = getCallback(); if (cb != null) cb.unscheduleDrawable(this, what); }
    public int getLayoutDirection() { return layoutDirection; }
    public final boolean setLayoutDirection(int dir) {
        if (layoutDirection == dir) return false;
        layoutDirection = dir;
        return onLayoutDirectionChanged(dir);
    }
    public boolean onLayoutDirectionChanged(int layoutDirection) { return false; }
    public abstract void setAlpha(int alpha);
    public int getAlpha() { return 0xff; }
    public abstract void setColorFilter(ColorFilter colorFilter);
    public void setColorFilter(int color, android.graphics.PorterDuff.Mode mode) {
        setColorFilter(new android.graphics.PorterDuffColorFilter(color, mode));
    }
    public void setTint(int tint) { setTintList(android.content.res.ColorStateList.valueOf(tint)); }
    public void setTintList(android.content.res.ColorStateList tint) {}
    public void setTintMode(android.graphics.PorterDuff.Mode tintMode) {}
    public void setTintBlendMode(android.graphics.BlendMode blendMode) {}
    public ColorFilter getColorFilter() { return null; }
    public void clearColorFilter() { setColorFilter(null); }
    public void setHotspot(float x, float y) {}
    public void setHotspotBounds(int left, int top, int right, int bottom) {}
    public void getHotspotBounds(Rect out) { out.set(getBounds()); }
    public boolean isProjected() { return false; }
    public boolean isStateful() { return false; }
    public boolean hasFocusStateSpecified() { return false; }
    public boolean setState(int[] stateSet) {
        if (!java.util.Arrays.equals(this.stateSet, stateSet)) {
            this.stateSet = stateSet;
            return onStateChange(stateSet);
        }
        return false;
    }
    public int[] getState() { return stateSet; }
    public void jumpToCurrentState() {}
    public Drawable getCurrent() { return this; }
    public final boolean setLevel(int level) {
        if (this.level != level) { this.level = level; return onLevelChange(level); }
        return false;
    }
    public final int getLevel() { return level; }
    public boolean setVisible(boolean visible, boolean restart) {
        boolean changed = this.visible != visible;
        if (changed) { this.visible = visible; invalidateSelf(); }
        return changed;
    }
    public final boolean isVisible() { return visible; }
    public void setAutoMirrored(boolean mirrored) {}
    public boolean isAutoMirrored() { return false; }
    public void applyTheme(Resources.Theme t) {}
    public boolean canApplyTheme() { return false; }
    @Deprecated public abstract int getOpacity();
    public static int resolveOpacity(int op1, int op2) {
        if (op1 == op2) return op1;
        if (op1 == PixelFormat.UNKNOWN || op2 == PixelFormat.UNKNOWN) return PixelFormat.UNKNOWN;
        if (op1 == PixelFormat.TRANSLUCENT || op2 == PixelFormat.TRANSLUCENT) return PixelFormat.TRANSLUCENT;
        if (op1 == PixelFormat.TRANSPARENT || op2 == PixelFormat.TRANSPARENT) return PixelFormat.TRANSPARENT;
        return PixelFormat.OPAQUE;
    }
    public android.graphics.Region getTransparentRegion() { return null; }
    protected boolean onStateChange(int[] state) { return false; }
    protected boolean onLevelChange(int level) { return false; }
    protected void onBoundsChange(Rect bounds) {}
    public int getIntrinsicWidth() { return -1; }
    public int getIntrinsicHeight() { return -1; }
    public int getMinimumWidth() { int w = getIntrinsicWidth(); return Math.max(w, 0); }
    public int getMinimumHeight() { int h = getIntrinsicHeight(); return Math.max(h, 0); }
    public boolean getPadding(Rect padding) { padding.set(0, 0, 0, 0); return false; }
    public Insets getOpticalInsets() { return Insets.NONE; }
    public void getOutline(Outline outline) { outline.setRect(getBounds()); outline.setAlpha(0); }
    public Drawable mutate() { return this; }
    public ConstantState getConstantState() { return null; }
}
