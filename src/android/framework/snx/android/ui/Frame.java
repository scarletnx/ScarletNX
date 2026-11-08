// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.ui;

import android.graphics.Bitmap;
import android.graphics.Matrix;
import android.graphics.Outline;
import android.graphics.Path;
import android.graphics.RenderNode;
import java.util.ArrayList;
import snx.android.hwui.DisplayList;

/**
 * A frame for the renderer: the tree of RenderNodes flattened into one display list, each node between NODE_BEGIN
 * (its transform in its parent, alpha, size and clips) and NODE_END (docs/platforms/android-apps/hwui.md).
 */
public final class Frame {
    private final DisplayList.Writer w = new DisplayList.Writer();
    private final ArrayList<Bitmap> bitmaps = new ArrayList<>();
    /** For each bitmap: the operations drawn into it since the last frame (null for none), and its pixels to upload */
    private final ArrayList<byte[]> drawings = new ArrayList<>();
    private final ArrayList<int[]> uploads = new ArrayList<>();
    private int nodes;
    public final int width, height;

    private Frame(int width, int height) { this.width = width; this.height = height; }

    public static Frame flatten(RenderNode root, int width, int height) {
        Frame f = new Frame(width, height);
        f.node(root);
        return f;
    }
    /** Windows' roots one over the other, the first at the bottom */
    public static Frame flatten(java.util.List<RenderNode> roots, int width, int height) {
        Frame f = new Frame(width, height);
        for (RenderNode root : roots) f.node(root);
        return f;
    }

    public int size() { return w.size(); }
    public int nodes() { return nodes; }
    public byte[] bytes() { return w.array(); }
    public java.util.List<Bitmap> bitmaps() { return bitmaps; }

    private void node(RenderNode n) {
        DisplayList list = n.displayList();
        if (list == null || n.getAlpha() <= 0) return;
        ++nodes;
        w.u8(DisplayList.NODE_BEGIN);
        Matrix m = new Matrix();
        n.transformInParent(m);
        for (float v : m.values()) w.f32(v);
        w.f32(n.getAlpha());
        w.f32(n.getWidth());
        w.f32(n.getHeight());
        w.u8(n.getClipToBounds() ? 1 : 0);
        Outline o = n.outline();
        if (n.getClipToOutline() && o.mMode == Outline.MODE_ROUND_RECT) {
            w.u8(1);
            w.f32(o.mRect.left); w.f32(o.mRect.top); w.f32(o.mRect.right); w.f32(o.mRect.bottom); w.f32(Math.max(0, o.mRadius));
        } else if (n.getClipToOutline() && o.mMode == Outline.MODE_PATH && o.mPath != null) {
            w.u8(2);
            path(o.mPath);
        } else {
            w.u8(0);
        }
        if (n.hasClipRect()) {
            w.u8(1);
            w.f32(n.clipRect().left); w.f32(n.clipRect().top); w.f32(n.clipRect().right); w.f32(n.clipRect().bottom);
        } else {
            w.u8(0);
        }
        for (Bitmap b : list.bitmaps()) bitmap(b);
        int segments = list.segmentCount();
        for (int i = 0; i < segments; ++i) {
            byte[] s = list.segment(i);
            w.bytes(s, 0, s.length);
            if (i < list.childCount()) node(list.child(i));
        }
        w.u8(DisplayList.NODE_END);
    }

    /**
     * A bitmap the frame draws, after the bitmaps that its own drawing draws: its pixels to upload, and what canvases
     * drew into it, flattened as a node's list is, for the renderer to draw into its image first.
     */
    private void bitmap(Bitmap b) {
        if (bitmaps.contains(b)) return;
        DisplayList d = b.drawingOrNull();
        byte[] ops = null;
        if (d != null && !d.isEmpty()) {
            d.finish();
            for (Bitmap inner : d.bitmaps()) bitmap(inner);
            Frame f = new Frame(b.getWidth(), b.getHeight());
            f.list(d);
            for (int i = 0; i < f.bitmaps.size(); ++i) {
                if (bitmaps.contains(f.bitmaps.get(i))) continue;
                bitmaps.add(f.bitmaps.get(i));
                drawings.add(f.drawings.get(i));
                uploads.add(f.uploads.get(i));
            }
            ops = java.util.Arrays.copyOf(f.w.array(), f.w.size());
            d.clear();
        }
        bitmaps.add(b);
        drawings.add(ops);
        uploads.add(b.takePixels());
    }

    /** A list's segments and its children, without a node of its own */
    private void list(DisplayList list) {
        for (Bitmap b : list.bitmaps()) bitmap(b);
        int segments = list.segmentCount();
        for (int i = 0; i < segments; ++i) {
            byte[] s = list.segment(i);
            w.bytes(s, 0, s.length);
            if (i < list.childCount()) node(list.child(i));
        }
    }

    public java.util.List<byte[]> drawings() { return drawings; }
    public java.util.List<int[]> uploads() { return uploads; }

    private void path(Path p) {
        w.u8(p.getFillType().ordinal());
        w.i32(p.verbCount());
        w.i32(p.pointCount());
        w.bytes(p.verbs(), 0, p.verbCount());
        for (int i = 0; i < p.pointCount(); ++i) w.f32(p.points()[i]);
    }
}
