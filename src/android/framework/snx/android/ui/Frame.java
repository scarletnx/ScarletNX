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
 * (its transform in its parent, alpha, size and clips) and NODE_END (docs/android/HWUI.md).
 */
public final class Frame {
    private final DisplayList.Writer w = new DisplayList.Writer();
    private final ArrayList<Bitmap> bitmaps = new ArrayList<>();
    private int nodes;
    public final int width, height;

    private Frame(int width, int height) { this.width = width; this.height = height; }

    public static Frame flatten(RenderNode root, int width, int height) {
        Frame f = new Frame(width, height);
        f.node(root);
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
        for (Bitmap b : list.bitmaps()) if (!bitmaps.contains(b)) bitmaps.add(b);
        int segments = list.segmentCount();
        for (int i = 0; i < segments; ++i) {
            byte[] s = list.segment(i);
            w.bytes(s, 0, s.length);
            if (i < list.childCount()) node(list.child(i));
        }
        w.u8(DisplayList.NODE_END);
    }

    private void path(Path p) {
        w.u8(p.getFillType().ordinal());
        w.i32(p.verbCount());
        w.i32(p.pointCount());
        w.bytes(p.verbs(), 0, p.verbCount());
        w.pad4();
        for (int i = 0; i < p.pointCount(); ++i) w.f32(p.points()[i]);
    }
}
