// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.ui;

import android.graphics.RenderNode;
import android.view.ViewRootImpl;
import java.util.ArrayList;
import java.util.HashMap;

/**
 * The host window the process's windows show in, one over the other: the last attached gets the input. A frame is
 * every root's last recording, the first attached at the bottom, as Android's compositor stacks an app's windows. It
 * is flattened (Frame) and drawn by the runtime's Vulkan renderer; without a window the frames are counted.
 */
public final class Host {
    private static final ArrayList<ViewRootImpl> roots = new ArrayList<>();
    private static final HashMap<ViewRootImpl, RenderNode> nodes = new HashMap<>();
    private static long frames;
    private static final boolean TRACE = Boolean.getBoolean("snx.android.trace_frames");

    private Host() {}

    public static synchronized void attach(ViewRootImpl root) {
        roots.add(root);
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
    public static synchronized ViewRootImpl top() { return roots.isEmpty() ? null : roots.get(roots.size() - 1); }
    public static long frames() { return frames; }

    /** A root's frame: drawn over the roots below it, and under the roots above it. */
    public static void draw(ViewRootImpl root, RenderNode node) {
        synchronized (Host.class) {
            nodes.put(root, node);
        }
        present();
    }

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
