// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.ui;

import android.graphics.RenderNode;
import android.view.ViewRootImpl;
import android.view.WindowManager;
import java.util.ArrayList;
import java.util.HashMap;

/**
 * The host window the process's windows show in, one over the other, stacked as attach orders them: the top one gets
 * the input. A frame is every root's last recording, the bottom one first, as Android's compositor stacks an app's
 * windows. It is flattened (Frame) and drawn by the runtime's Vulkan renderer; without a window the frames are
 * counted.
 */
public final class Host {
    private static final ArrayList<ViewRootImpl> roots = new ArrayList<>();
    private static final HashMap<ViewRootImpl, RenderNode> nodes = new HashMap<>();
    private static long frames;
    private static final boolean TRACE = Boolean.getBoolean("snx.android.trace_frames");

    private Host() {}

    /**
     * A window goes over the others, except an activity's own window: under the windows of its token that it already
     * has (a dialog its onCreate showed), as WindowManager stacks a token's base window below its other windows.
     */
    public static synchronized void attach(ViewRootImpl root) {
        int at = roots.size();
        WindowManager.LayoutParams a = root.getWindowAttributes();
        if (a != null && a.type == WindowManager.LayoutParams.TYPE_BASE_APPLICATION && a.token != null) {
            for (int i = 0; i < roots.size() && at == roots.size(); ++i) {
                WindowManager.LayoutParams b = roots.get(i).getWindowAttributes();
                if (b != null && b.token == a.token) at = i;
            }
        }
        roots.add(at, root);
        if (TRACE) android.util.Log.d("snx", "window " + root + " attached, " + roots.size() + " now");
    }
    /** A root gone: the roots left drawn again, without it. */
    public static void detach(ViewRootImpl root) {
        boolean drew;
        synchronized (Host.class) {
            roots.remove(root);
            drew = nodes.remove(root) != null;
            if (TRACE) android.util.Log.d("snx", "window " + root + " detached, " + roots.size() + " left");
        }
        if (drew) present();
    }
    /** Every window's root laid out at the display's new size, on the main thread */
    public static void resize(int width, int height) {
        if (android.os.Looper.myLooper() != android.os.Looper.getMainLooper()) {
            new android.os.Handler(android.os.Looper.getMainLooper()).post(() -> resize(width, height));
            return;
        }
        ViewRootImpl[] all;
        synchronized (Host.class) { all = roots.toArray(new ViewRootImpl[0]); }
        for (ViewRootImpl r : all) r.resize(width, height);
    }
    /* the window a touch's down went to: its moves and its up go there too, as Android's input dispatcher keeps a
       gesture on the window that took it */
    private static ViewRootImpl touched;
    /**
     * The window a touch at (x, y) of the display goes to, as Android's input dispatcher picks it: from the top, the
     * first whose frame holds the point and takes touches, or the first touch-modal one, which takes every touch (a
     * dialog's). A window that does not take focus (FLAG_NOT_FOCUSABLE) or sets FLAG_NOT_TOUCH_MODAL lets the touches
     * outside it through to the windows below (a popup's). The windows above it that watch outside touches
     * (FLAG_WATCH_OUTSIDE_TOUCH) get an ACTION_OUTSIDE of the down, which outside.
     */
    public static ViewRootImpl touchTarget(float x, float y, int action, java.util.List<ViewRootImpl> outside) {
        synchronized (Host.class) {
            if (action != android.view.MotionEvent.ACTION_DOWN && touched != null && roots.contains(touched))
                return touched;
            touched = null;
            for (int i = roots.size() - 1; i >= 0 && touched == null; --i) {
                ViewRootImpl r = roots.get(i);
                int flags = r.windowFlags();
                boolean takes = (flags & WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE) == 0;
                boolean modal = (flags & (WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                                          | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL)) == 0;
                if (takes && (r.frameContains(x, y) || modal)) touched = r;
                else if (action == android.view.MotionEvent.ACTION_DOWN
                         && (flags & WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH) != 0) outside.add(r);
            }
            return touched;
        }
    }
    /** The window a key goes to: the top one that takes focus */
    public static synchronized ViewRootImpl keyTarget() {
        for (int i = roots.size() - 1; i >= 0; --i)
            if ((roots.get(i).windowFlags() & WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE) == 0) return roots.get(i);
        return null;
    }
    /** A touch of the host window or the script given to the window it goes to (touchTarget), on the main thread */
    static void touch(android.view.MotionEvent e) {
        java.util.ArrayList<ViewRootImpl> outside = new java.util.ArrayList<>();
        ViewRootImpl root = touchTarget(e.getX(), e.getY(), e.getActionMasked(), outside);
        for (ViewRootImpl r : outside) {
            android.view.MotionEvent o = android.view.MotionEvent.obtain(e);
            o.setAction(android.view.MotionEvent.ACTION_OUTSIDE);
            r.dispatchInputEvent(o);
        }
        if (root != null) root.dispatchInputEvent(e);
    }
    public static long frames() { return frames; }

    /** A root's frame: drawn over the roots below it, and under the roots above it. */
    public static void draw(ViewRootImpl root, RenderNode node) {
        synchronized (Host.class) {
            nodes.put(root, node);
        }
        present();
    }

    /** the last frame again, for a display that keeps showing it (Window's scripts) */
    static void refresh() { present(); }

    private static void present() {
        Frame frame;
        synchronized (Host.class) {
            ArrayList<RenderNode> list = new ArrayList<>();
            int width = 0, height = 0;
            for (ViewRootImpl r : roots) {
                RenderNode n = nodes.get(r);
                if (n == null) continue;
                list.add(n);
                width = Math.max(width, r.width());
                height = Math.max(height, r.height());
            }
            if (list.isEmpty()) return;
            frame = Frame.flatten(list, width, height);
            ++frames;
        }
        if (TRACE) android.util.Log.d("snx", "frame " + frames + ": " + frame.size() + " bytes, " + frame.nodes() + " nodes");
        Window.present(frame);
    }
}
