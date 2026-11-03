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
import java.util.Arrays;

/** A drawable that shows one of its children at a time. */
public class DrawableContainer extends Drawable implements Drawable.Callback {
    private DrawableContainerState state;
    private Drawable current;
    private int currentIndex = -1, alpha = 0xff;
    private ColorFilter filter;

    public DrawableContainer() {}

    public abstract static class DrawableContainerState extends ConstantState {
        final DrawableContainer owner;
        Drawable[] drawables = new Drawable[10];
        int count, changingConfigurations;
        boolean constantSize, variablePadding;

        DrawableContainerState(DrawableContainerState orig, DrawableContainer owner, Resources res) {
            this.owner = owner;
            if (orig != null) {
                count = orig.count;
                drawables = new Drawable[orig.drawables.length];
                for (int i = 0; i < count; ++i) {
                    Drawable d = orig.drawables[i];
                    ConstantState cs = d != null ? d.getConstantState() : null;
                    drawables[i] = cs != null ? cs.newDrawable(res) : d;
                    if (drawables[i] != null && owner != null) drawables[i].setCallback(owner);
                }
                constantSize = orig.constantSize;
                variablePadding = orig.variablePadding;
            }
        }
        @Override public int getChangingConfigurations() { return changingConfigurations; }
        public final int addChild(Drawable dr) {
            if (count >= drawables.length) drawables = Arrays.copyOf(drawables, count * 2);
            dr.mutate();
            dr.setVisible(false, true);
            dr.setCallback(owner);
            drawables[count] = dr;
            changingConfigurations |= dr.getChangingConfigurations();
            return count++;
        }
        public final int getCapacity() { return drawables.length; }
        public final int getChildCount() { return count; }
        public final Drawable[] getChildren() { return drawables; }
        public final Drawable getChild(int index) { return index >= 0 && index < count ? drawables[index] : null; }
        public final void setVariablePadding(boolean variable) { variablePadding = variable; }
        public final void setConstantSize(boolean constant) { constantSize = constant; }
        public final boolean isConstantSize() { return constantSize; }
        public final int getConstantWidth() {
            int w = -1;
            for (int i = 0; i < count; ++i) w = Math.max(w, drawables[i].getIntrinsicWidth());
            return w;
        }
        public final int getConstantHeight() {
            int h = -1;
            for (int i = 0; i < count; ++i) h = Math.max(h, drawables[i].getIntrinsicHeight());
            return h;
        }
        public final boolean isStateful() {
            for (int i = 0; i < count; ++i) if (drawables[i].isStateful()) return true;
            return false;
        }
    }

    protected void setConstantState(DrawableContainerState state) { this.state = state; }
    DrawableContainerState containerState() { return state; }

    public boolean selectDrawable(int index) {
        if (index == currentIndex) return false;
        if (current != null) current.setVisible(false, false);
        if (state != null && index >= 0 && index < state.count) {
            current = state.drawables[index];
            currentIndex = index;
            current.setVisible(isVisible(), true);
            current.setAlpha(alpha);
            if (filter != null) current.setColorFilter(filter);
            current.setState(getState());
            current.setLevel(getLevel());
            current.setBounds(getBounds());
            current.setLayoutDirection(getLayoutDirection());
        } else {
            current = null;
            currentIndex = -1;
        }
        invalidateSelf();
        return true;
    }
    int currentIndex() { return currentIndex; }

    @Override public Drawable getCurrent() { return current; }
    @Override public void draw(Canvas canvas) { if (current != null) current.draw(canvas); }
    @Override public void setAlpha(int alpha) {
        if (this.alpha != alpha) {
            this.alpha = alpha;
            if (current != null) current.setAlpha(alpha);
        }
    }
    @Override public int getAlpha() { return alpha; }
    @Override public void setColorFilter(ColorFilter colorFilter) {
        filter = colorFilter;
        if (current != null) current.setColorFilter(colorFilter);
    }
    @Override public ColorFilter getColorFilter() { return filter; }
    @Override public int getOpacity() { return current != null && current.isVisible() ? current.getOpacity() : PixelFormat.TRANSPARENT; }
    @Override public boolean isStateful() { return state != null && state.isStateful(); }
    @Override public void jumpToCurrentState() { if (current != null) current.jumpToCurrentState(); }
    @Override protected boolean onStateChange(int[] state) { return current != null && current.setState(state); }
    @Override protected boolean onLevelChange(int level) { return current != null && current.setLevel(level); }
    @Override protected void onBoundsChange(Rect bounds) { if (current != null) current.setBounds(bounds); }
    @Override public boolean onLayoutDirectionChanged(int layoutDirection) { return current != null && current.setLayoutDirection(layoutDirection); }
    @Override public boolean setVisible(boolean visible, boolean restart) {
        boolean changed = super.setVisible(visible, restart);
        if (current != null) current.setVisible(visible, restart);
        return changed;
    }
    @Override public int getIntrinsicWidth() {
        if (state != null && state.constantSize) return state.getConstantWidth();
        return current != null ? current.getIntrinsicWidth() : -1;
    }
    @Override public int getIntrinsicHeight() {
        if (state != null && state.constantSize) return state.getConstantHeight();
        return current != null ? current.getIntrinsicHeight() : -1;
    }
    @Override public boolean getPadding(Rect padding) { return current != null ? current.getPadding(padding) : super.getPadding(padding); }
    @Override public Insets getOpticalInsets() { return current != null ? current.getOpticalInsets() : Insets.NONE; }
    @Override public void getOutline(Outline outline) { if (current != null) current.getOutline(outline); }
    @Override public void setHotspot(float x, float y) { if (current != null) current.setHotspot(x, y); }
    @Override public ConstantState getConstantState() { return state; }

    @Override public void invalidateDrawable(Drawable who) { if (who == current) invalidateSelf(); }
    @Override public void scheduleDrawable(Drawable who, Runnable what, long when) { if (who == current) scheduleSelf(what, when); }
    @Override public void unscheduleDrawable(Drawable who, Runnable what) { if (who == current) unscheduleSelf(what); }
}
