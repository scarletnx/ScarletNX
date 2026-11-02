// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import java.util.concurrent.CopyOnWriteArrayList;

/** Listeners of a window's view tree: layout, drawing, focus, scrolling. */
public final class ViewTreeObserver {
    public interface OnGlobalFocusChangeListener { void onGlobalFocusChanged(View oldFocus, View newFocus); }
    public interface OnGlobalLayoutListener { void onGlobalLayout(); }
    public interface OnPreDrawListener { boolean onPreDraw(); }
    public interface OnDrawListener { void onDraw(); }
    public interface OnTouchModeChangeListener { void onTouchModeChanged(boolean isInTouchMode); }
    public interface OnScrollChangedListener { void onScrollChanged(); }
    public interface OnWindowAttachListener { void onWindowAttached(); void onWindowDetached(); }
    public interface OnWindowFocusChangeListener { void onWindowFocusChanged(boolean hasFocus); }
    public interface OnWindowVisibilityChangeListener { void onWindowVisibilityChanged(int visibility); }

    private final CopyOnWriteArrayList<OnGlobalFocusChangeListener> focus = new CopyOnWriteArrayList<>();
    private final CopyOnWriteArrayList<OnGlobalLayoutListener> layout = new CopyOnWriteArrayList<>();
    private final CopyOnWriteArrayList<OnPreDrawListener> preDraw = new CopyOnWriteArrayList<>();
    private final CopyOnWriteArrayList<OnDrawListener> draw = new CopyOnWriteArrayList<>();
    private final CopyOnWriteArrayList<OnTouchModeChangeListener> touchMode = new CopyOnWriteArrayList<>();
    private final CopyOnWriteArrayList<OnScrollChangedListener> scroll = new CopyOnWriteArrayList<>();
    private final CopyOnWriteArrayList<OnWindowAttachListener> attach = new CopyOnWriteArrayList<>();
    private final CopyOnWriteArrayList<OnWindowFocusChangeListener> windowFocus = new CopyOnWriteArrayList<>();
    private final CopyOnWriteArrayList<OnWindowVisibilityChangeListener> visibility = new CopyOnWriteArrayList<>();
    private boolean alive = true;

    ViewTreeObserver() {}

    void merge(ViewTreeObserver o) {
        focus.addAll(o.focus); layout.addAll(o.layout); preDraw.addAll(o.preDraw); draw.addAll(o.draw);
        touchMode.addAll(o.touchMode); scroll.addAll(o.scroll); attach.addAll(o.attach); windowFocus.addAll(o.windowFocus);
        visibility.addAll(o.visibility);
        o.alive = false;
    }

    public boolean isAlive() { return alive; }
    public void addOnGlobalFocusChangeListener(OnGlobalFocusChangeListener l) { focus.add(l); }
    public void removeOnGlobalFocusChangeListener(OnGlobalFocusChangeListener l) { focus.remove(l); }
    public void addOnGlobalLayoutListener(OnGlobalLayoutListener l) { layout.add(l); }
    @Deprecated public void removeGlobalOnLayoutListener(OnGlobalLayoutListener l) { layout.remove(l); }
    public void removeOnGlobalLayoutListener(OnGlobalLayoutListener l) { layout.remove(l); }
    public void addOnPreDrawListener(OnPreDrawListener l) { preDraw.add(l); }
    public void removeOnPreDrawListener(OnPreDrawListener l) { preDraw.remove(l); }
    public void addOnDrawListener(OnDrawListener l) { draw.add(l); }
    public void removeOnDrawListener(OnDrawListener l) { draw.remove(l); }
    public void addOnTouchModeChangeListener(OnTouchModeChangeListener l) { touchMode.add(l); }
    public void removeOnTouchModeChangeListener(OnTouchModeChangeListener l) { touchMode.remove(l); }
    public void addOnScrollChangedListener(OnScrollChangedListener l) { scroll.add(l); }
    public void removeOnScrollChangedListener(OnScrollChangedListener l) { scroll.remove(l); }
    public void addOnWindowAttachListener(OnWindowAttachListener l) { attach.add(l); }
    public void removeOnWindowAttachListener(OnWindowAttachListener l) { attach.remove(l); }
    public void addOnWindowFocusChangeListener(OnWindowFocusChangeListener l) { windowFocus.add(l); }
    public void removeOnWindowFocusChangeListener(OnWindowFocusChangeListener l) { windowFocus.remove(l); }
    public void addOnWindowVisibilityChangeListener(OnWindowVisibilityChangeListener l) { visibility.add(l); }
    public void removeOnWindowVisibilityChangeListener(OnWindowVisibilityChangeListener l) { visibility.remove(l); }
    public void registerFrameCommitCallback(Runnable callback) { Choreographer.getInstance().postFrameCallback(t -> callback.run()); }
    public boolean unregisterFrameCommitCallback(Runnable callback) { return false; }

    public void dispatchOnGlobalLayout() { for (OnGlobalLayoutListener l : layout) l.onGlobalLayout(); }
    public boolean dispatchOnPreDraw() {
        boolean cancel = false;
        for (OnPreDrawListener l : preDraw) cancel |= !l.onPreDraw();
        return cancel;
    }
    public void dispatchOnDraw() { for (OnDrawListener l : draw) l.onDraw(); }
    void dispatchOnGlobalFocusChange(View oldFocus, View newFocus) { for (OnGlobalFocusChangeListener l : focus) l.onGlobalFocusChanged(oldFocus, newFocus); }
    void dispatchOnScrollChanged() { for (OnScrollChangedListener l : scroll) l.onScrollChanged(); }
    void dispatchOnWindowFocusChange(boolean hasFocus) { for (OnWindowFocusChangeListener l : windowFocus) l.onWindowFocusChanged(hasFocus); }
    void dispatchOnWindowAttachedChange(boolean attached) { for (OnWindowAttachListener l : attach) { if (attached) l.onWindowAttached(); else l.onWindowDetached(); } }
}
