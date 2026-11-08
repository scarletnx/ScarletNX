// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.hwui;

import android.graphics.Bitmap;
import android.graphics.RenderNode;
import java.util.ArrayList;

/**
 * Drawing operations as bytes, the HLE of libhwui's display lists. A node's list is segments of operations with the
 * child nodes it draws between them, so a frame flattens the tree without parsing an operation. The format is
 * little-endian whatever the host is, as src/android/hwui/render.c reads it. docs/platforms/android-apps/hwui.md lists the
 * operations.
 */
public final class DisplayList {
    public static final byte SAVE = 1, RESTORE = 2, TRANSLATE = 4, SCALE = 5, ROTATE = 6, CONCAT = 7,
        SET_MATRIX = 8, CLIP_RECT = 10, CLIP_PATH = 11, DRAW_COLOR = 12, DRAW_PAINT = 13, DRAW_RECT = 14,
        DRAW_RRECT = 15, DRAW_OVAL = 16, DRAW_CIRCLE = 17, DRAW_ARC = 18, DRAW_LINES = 19, DRAW_POINTS = 20,
        DRAW_PATH = 21, DRAW_GLYPHS = 22, DRAW_BITMAP = 23, SAVE_LAYER = 25, DRAW_DRRECT = 26, NODE_BEGIN = 30,
        NODE_END = 31;

    /** The operations, and the children between them: segments.get(i) comes before children.get(i). */
    final ArrayList<byte[]> segments = new ArrayList<>();
    final ArrayList<RenderNode> children = new ArrayList<>();
    final ArrayList<Bitmap> bitmaps = new ArrayList<>();
    private final Writer current = new Writer();

    public Writer writer() { return current; }
    public boolean isEmpty() { return segments.isEmpty() && current.size() == 0 && children.isEmpty(); }

    public void clear() {
        segments.clear();
        children.clear();
        bitmaps.clear();
        current.reset();
    }

    public void addChild(RenderNode node) {
        segments.add(current.toArray());
        current.reset();
        children.add(node);
    }

    /** Another list's operations here, as one child: a Picture drawn. */
    public void addChildList(DisplayList list) {
        segments.add(current.toArray());
        current.reset();
        children.add(RenderNode.ofList(list));
    }

    public void addBitmap(Bitmap b) { if (!bitmaps.contains(b)) bitmaps.add(b); }

    /** The operations after the last child, made a segment. */
    public void finish() {
        segments.add(current.toArray());
        current.reset();
    }

    public int segmentCount() { return segments.size(); }
    public byte[] segment(int i) { return segments.get(i); }
    public int childCount() { return children.size(); }
    public RenderNode child(int i) { return children.get(i); }
    public java.util.List<Bitmap> bitmaps() { return bitmaps; }

    /** A little-endian byte writer. */
    public static final class Writer {
        private byte[] data = new byte[256];
        private int size;

        public int size() { return size; }
        public void reset() { size = 0; }
        public byte[] array() { return data; }
        public byte[] toArray() { return java.util.Arrays.copyOf(data, size); }
        private void need(int n) { if (size + n > data.length) data = java.util.Arrays.copyOf(data, Math.max(data.length * 2, size + n)); }
        public void u8(int v) { need(1); data[size++] = (byte) v; }
        public void i32(int v) {
            need(4);
            data[size++] = (byte) v; data[size++] = (byte) (v >> 8); data[size++] = (byte) (v >> 16); data[size++] = (byte) (v >> 24);
        }
        public void f32(float v) { i32(Float.floatToRawIntBits(v)); }
        public void u16(int v) { need(2); data[size++] = (byte) v; data[size++] = (byte) (v >> 8); }
        public void bytes(byte[] b, int off, int n) { need(n); System.arraycopy(b, off, data, size, n); size += n; }
        public void pad4() { while ((size & 3) != 0) u8(0); }
    }
}
