// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.ui;

import android.graphics.RenderNode;
import android.view.ViewRootImpl;
import java.util.ArrayList;

/**
 * The host window the process's windows show in, one over the other: the last attached gets the input. A frame of
 * a root is flattened (Frame) and drawn by the runtime's Vulkan renderer; without a window the frames are counted.
 */
public final class Host {
    private static final ArrayList<ViewRootImpl> roots = new ArrayList<>();
    private static long frames;
    private static final boolean TRACE = System.getenv("SNX_ANDROID_TRACE_FRAMES") != null;

    private Host() {}

    public static synchronized void attach(ViewRootImpl root) { roots.add(root); }
    public static synchronized void detach(ViewRootImpl root) { roots.remove(root); }
    public static synchronized ViewRootImpl top() { return roots.isEmpty() ? null : roots.get(roots.size() - 1); }
    public static long frames() { return frames; }

    /** A root's frame: drawn, under the roots below it. */
    public static void draw(ViewRootImpl root, RenderNode node) {
        Frame frame = Frame.flatten(node, root.width(), root.height());
        ++frames;
        if (TRACE) android.util.Log.d("snx", "frame " + frames + ": " + frame.size() + " bytes, " + frame.nodes() + " nodes");
        Window.present(frame);
    }
}
