// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.graphics.drawable;

import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.view.Gravity;
import java.util.ArrayList;

/** Drawables drawn one over another, each in the bounds inset by its own insets. */
public class LayerDrawable extends Drawable implements Drawable.Callback {
    public static final int PADDING_MODE_NEST = 0, PADDING_MODE_STACK = 1;
    public static final int INSET_UNDEFINED = Integer.MIN_VALUE;

    private static final class Layer {
        Drawable drawable;
        int id = -1, gravity = Gravity.NO_GRAVITY, width = -1, height = -1;
        int left, top, right, bottom, start = INSET_UNDEFINED, end = INSET_UNDEFINED;
    }

    private final ArrayList<Layer> layers = new ArrayList<>();
    private final Rect tmp = new Rect(), container = new Rect();
    private int alpha = 0xff, paddingMode = PADDING_MODE_NEST;

    public LayerDrawable(Drawable[] layers) {
        if (layers == null) throw new IllegalArgumentException("layers must be non-null");
        for (Drawable d : layers) addLayer(d);
    }
    LayerDrawable() {}

    public int addLayer(Drawable dr) {
        Layer l = new Layer();
        l.drawable = dr;
        if (dr != null) {
            dr.setCallback(this);
            dr.setLayoutDirection(getLayoutDirection());
        }
        layers.add(l);
        refresh(l);
        return layers.size() - 1;
    }
    private Layer layer(int index) {
        if (index < 0 || index >= layers.size()) throw new IndexOutOfBoundsException("Invalid layer index " + index);
        return layers.get(index);
    }
    private void refresh(Layer l) {
        if (l.drawable != null) {
            updateBounds(l, getBounds());
            l.drawable.setState(getState());
            l.drawable.setLevel(getLevel());
        }
        invalidateSelf();
    }
    public int getNumberOfLayers() { return layers.size(); }
    public Drawable getDrawable(int index) { return layer(index).drawable; }
    public void setDrawable(int index, Drawable drawable) {
        Layer l = layer(index);
        if (l.drawable != null) l.drawable.setCallback(null);
        l.drawable = drawable;
        if (drawable != null) drawable.setCallback(this);
        refresh(l);
    }
    public Drawable findDrawableByLayerId(int id) {
        for (int i = layers.size() - 1; i >= 0; --i) if (layers.get(i).id == id) return layers.get(i).drawable;
        return null;
    }
    public int findIndexByLayerId(int id) {
        for (int i = 0; i < layers.size(); ++i) if (layers.get(i).id == id) return i;
        return -1;
    }
    public boolean setDrawableByLayerId(int id, Drawable drawable) {
        int i = findIndexByLayerId(id);
        if (i < 0) return false;
        setDrawable(i, drawable);
        return true;
    }
    public void setId(int index, int id) { layer(index).id = id; }
    public int getId(int index) { return layer(index).id; }
    public void setLayerGravity(int index, int gravity) { layer(index).gravity = gravity; refresh(layer(index)); }
    public int getLayerGravity(int index) { return layer(index).gravity; }
    public void setLayerWidth(int index, int w) { layer(index).width = w; refresh(layer(index)); }
    public int getLayerWidth(int index) { return layer(index).width; }
    public void setLayerHeight(int index, int h) { layer(index).height = h; refresh(layer(index)); }
    public int getLayerHeight(int index) { return layer(index).height; }
    public void setLayerSize(int index, int w, int h) { Layer l = layer(index); l.width = w; l.height = h; refresh(l); }
    public void setLayerInset(int index, int l, int t, int r, int b) { setLayerInsetInternal(index, l, t, r, b, INSET_UNDEFINED, INSET_UNDEFINED); }
    public void setLayerInsetRelative(int index, int s, int t, int e, int b) { setLayerInsetInternal(index, 0, t, 0, b, s, e); }
    private void setLayerInsetInternal(int index, int l, int t, int r, int b, int s, int e) {
        Layer layer = layer(index);
        layer.left = l; layer.top = t; layer.right = r; layer.bottom = b; layer.start = s; layer.end = e;
        refresh(layer);
    }
    public void setLayerInsetLeft(int index, int l) { layer(index).left = l; refresh(layer(index)); }
    public int getLayerInsetLeft(int index) { return layer(index).left; }
    public void setLayerInsetRight(int index, int r) { layer(index).right = r; refresh(layer(index)); }
    public int getLayerInsetRight(int index) { return layer(index).right; }
    public void setLayerInsetTop(int index, int t) { layer(index).top = t; refresh(layer(index)); }
    public int getLayerInsetTop(int index) { return layer(index).top; }
    public void setLayerInsetBottom(int index, int b) { layer(index).bottom = b; refresh(layer(index)); }
    public int getLayerInsetBottom(int index) { return layer(index).bottom; }
    public void setLayerInsetStart(int index, int s) { layer(index).start = s; refresh(layer(index)); }
    public int getLayerInsetStart(int index) { return layer(index).start; }
    public void setLayerInsetEnd(int index, int e) { layer(index).end = e; refresh(layer(index)); }
    public int getLayerInsetEnd(int index) { return layer(index).end; }
    public void setPaddingMode(int mode) { paddingMode = mode; }
    public int getPaddingMode() { return paddingMode; }

    private void updateBounds(Layer l, Rect bounds) {
        boolean rtl = getLayoutDirection() == 1;
        int left = l.left, right = l.right;
        if (l.start != INSET_UNDEFINED) { if (rtl) right = l.start; else left = l.start; }
        if (l.end != INSET_UNDEFINED) { if (rtl) left = l.end; else right = l.end; }
        container.set(bounds.left + left, bounds.top + l.top, bounds.right - right, bounds.bottom - l.bottom);
        Drawable d = l.drawable;
        int w = l.width >= 0 ? l.width : d.getIntrinsicWidth(), h = l.height >= 0 ? l.height : d.getIntrinsicHeight();
        int gravity = l.gravity;
        if (gravity == Gravity.NO_GRAVITY) /* a layer without a gravity fills, unless the app gave its size */
            gravity = (l.width < 0 ? Gravity.FILL_HORIZONTAL : Gravity.START) | (l.height < 0 ? Gravity.FILL_VERTICAL : Gravity.TOP);
        Gravity.apply(gravity, Math.max(w, 0), Math.max(h, 0), container, tmp, getLayoutDirection());
        d.setBounds(tmp);
    }
    @Override protected void onBoundsChange(Rect bounds) {
        for (Layer l : layers) if (l.drawable != null) updateBounds(l, bounds);
    }
    @Override public void draw(Canvas canvas) {
        for (Layer l : layers) if (l.drawable != null) l.drawable.draw(canvas);
    }
    @Override public void setAlpha(int alpha) {
        this.alpha = alpha;
        for (Layer l : layers) if (l.drawable != null) l.drawable.setAlpha(alpha);
    }
    @Override public int getAlpha() { return alpha; }
    @Override public void setColorFilter(ColorFilter colorFilter) {
        for (Layer l : layers) if (l.drawable != null) l.drawable.setColorFilter(colorFilter);
    }
    @Override public int getOpacity() {
        int op = PixelFormat.TRANSPARENT;
        boolean first = true;
        for (Layer l : layers) {
            if (l.drawable == null) continue;
            op = first ? l.drawable.getOpacity() : resolveOpacity(op, l.drawable.getOpacity());
            first = false;
        }
        return op;
    }
    @Override public boolean isStateful() {
        for (Layer l : layers) if (l.drawable != null && l.drawable.isStateful()) return true;
        return false;
    }
    @Override protected boolean onStateChange(int[] state) {
        boolean changed = false;
        for (Layer l : layers) if (l.drawable != null && l.drawable.isStateful() && l.drawable.setState(state)) changed = true;
        if (changed) onBoundsChange(getBounds());
        return changed;
    }
    @Override protected boolean onLevelChange(int level) {
        boolean changed = false;
        for (Layer l : layers) if (l.drawable != null && l.drawable.setLevel(level)) changed = true;
        if (changed) onBoundsChange(getBounds());
        return changed;
    }
    @Override public boolean setVisible(boolean visible, boolean restart) {
        boolean changed = super.setVisible(visible, restart);
        for (Layer l : layers) if (l.drawable != null) l.drawable.setVisible(visible, restart);
        return changed;
    }
    @Override public void jumpToCurrentState() { for (Layer l : layers) if (l.drawable != null) l.drawable.jumpToCurrentState(); }
    @Override public void setHotspot(float x, float y) { for (Layer l : layers) if (l.drawable != null) l.drawable.setHotspot(x, y); }
    @Override public boolean onLayoutDirectionChanged(int layoutDirection) {
        for (Layer l : layers) if (l.drawable != null) l.drawable.setLayoutDirection(layoutDirection);
        onBoundsChange(getBounds());
        return true;
    }
    @Override public int getIntrinsicWidth() {
        int w = -1;
        for (Layer l : layers) {
            if (l.drawable == null) continue;
            int lw = l.width >= 0 ? l.width : l.drawable.getIntrinsicWidth();
            int start = l.start != INSET_UNDEFINED ? l.start : l.left, end = l.end != INSET_UNDEFINED ? l.end : l.right;
            if (lw >= 0) w = Math.max(w, lw + start + end);
        }
        return w;
    }
    @Override public int getIntrinsicHeight() {
        int h = -1;
        for (Layer l : layers) {
            if (l.drawable == null) continue;
            int lh = l.height >= 0 ? l.height : l.drawable.getIntrinsicHeight();
            if (lh >= 0) h = Math.max(h, lh + l.top + l.bottom);
        }
        return h;
    }
    @Override public Drawable mutate() {
        for (Layer l : layers) if (l.drawable != null) l.drawable.mutate();
        return this;
    }

    @Override public void invalidateDrawable(Drawable who) { invalidateSelf(); }
    @Override public void scheduleDrawable(Drawable who, Runnable what, long when) { scheduleSelf(what, when); }
    @Override public void unscheduleDrawable(Drawable who, Runnable what) { unscheduleSelf(what); }
}
