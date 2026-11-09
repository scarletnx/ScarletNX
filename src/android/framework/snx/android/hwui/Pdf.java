// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.hwui;

import android.graphics.Bitmap;
import android.graphics.Matrix;
import android.graphics.Outline;
import android.graphics.Path;
import android.graphics.RenderNode;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.BitSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;
import java.util.zip.Deflater;

/**
 * A PDF document of display lists, the HLE of Skia's PDF backend (SkPDF), which Android's PdfDocument draws through.
 * Each page's operations (docs/platforms/android-apps/hwui.md) become PDF's own: paths filled and stroked with the paint's color and
 * alpha, clips, transforms, images of the bitmaps' pixels, and glyphs in their font, which the file carries (only the
 * glyphs the pages use) with a map back to the text. A page draws as the renderer draws it: gradients and bitmap
 * shaders are their colors' average, a clip of difference does not clip, and a layer multiplies its alpha into what it
 * holds.
 */
public final class Pdf {
    private static final float KAPPA = 0.5522848f; // a quarter circle's cubic, its control points at this of the radius

    /** A page's content stream and size */
    private record Page(byte[] content, int width, int height) {}

    /** A font the pages draw: its file, face, and the glyphs used */
    private static final class Font {
        final String path;
        final int index;
        final BitSet glyphs = new BitSet();

        Font(String path, int index) {
            this.path = path;
            this.index = index;
        }
    }

    /** An image of a bitmap's pixels: RGB and alpha planes */
    private record Image(int width, int height, byte[] rgb, byte[] alpha) {}

    private final List<Page> pages = new ArrayList<>();
    private final Map<Integer, Font> fonts = new LinkedHashMap<>();
    private final List<Image> images = new ArrayList<>();
    private final Map<Integer, Integer> imageOf = new LinkedHashMap<>(); // a bitmap's id and its tint: its image
    private final TreeMap<Integer, Integer> alphas = new TreeMap<>(); // an alpha (0 to 255): its graphics state

    /** A page of width by height, its drawing the list's */
    public void addPage(DisplayList list, int width, int height) {
        Writer out = new Writer(height);
        list.finish();
        out.list(list);
        pages.add(new Page(out.content(), width, height));
    }

    public int pageCount() { return pages.size(); }

    // ---- a page's content ----

    /** The drawing of one page into its content stream */
    private final class Writer {
        private final StringBuilder s = new StringBuilder();
        /** The alpha that nodes and layers multiply into what they hold */
        private final ArrayList<Float> alphaStack = new ArrayList<>();
        /** The canvas's transform since its node's, as SET_MATRIX replaces it: a b c d e f, as PDF's cm */
        private final ArrayList<float[]> matrices = new ArrayList<>();
        private float alpha = 1;
        private float[] matrix = identity();
        /** The depth of the stack where the list being drawn began: its restores stop there */
        private int floor;
        /** The bitmaps the lists draw, by id */
        private final Map<Integer, Bitmap> bitmaps = new java.util.HashMap<>();

        Writer(int height) {
            // Android's coordinates have y down: a page's top left is its origin
            s.append("1 0 0 -1 0 ").append(height).append(" cm\n");
        }

        byte[] content() { return s.toString().getBytes(StandardCharsets.ISO_8859_1); }

        /** A list's segments and the children between them; a save in one segment may be restored in another */
        void list(DisplayList list) {
            for (Bitmap b : list.bitmaps()) bitmaps.put(b.id(), b);
            int outer = floor;
            floor = alphaStack.size();
            for (int i = 0; i < list.segmentCount(); ++i) {
                ops(list.segment(i));
                if (i < list.childCount()) node(list.child(i));
            }
            while (alphaStack.size() > floor) pop();
            floor = outer;
        }

        private void node(RenderNode n) {
            DisplayList list = n.displayList();
            if (list == null || n.getAlpha() <= 0) return;
            push();
            Matrix m = new Matrix();
            n.transformInParent(m);
            cm(fromAndroid(values(m)));
            matrix = identity();
            alpha *= n.getAlpha();
            if (n.getClipToBounds()) clip(rect(0, 0, n.getWidth(), n.getHeight()), false);
            Outline o = n.outline();
            if (n.getClipToOutline() && o.mMode == Outline.MODE_ROUND_RECT) {
                float r = Math.max(0, o.mRadius);
                clip(roundRect(o.mRect.left, o.mRect.top, o.mRect.right, o.mRect.bottom, r, r), false);
            } else if (n.getClipToOutline() && o.mMode == Outline.MODE_PATH && o.mPath != null) {
                ByteBuffer b = ByteBuffer.wrap(pathBytes(o.mPath)).order(ByteOrder.LITTLE_ENDIAN);
                int fill = b.get();
                clip(path(b), fill == 1 || fill == 3);
            }
            if (n.hasClipRect())
                clip(rect(n.clipRect().left, n.clipRect().top, n.clipRect().right, n.clipRect().bottom), false);
            list.finish();
            list(list);
            pop();
        }

        private void push() {
            s.append("q\n");
            alphaStack.add(alpha);
            matrices.add(matrix);
        }

        private void pop() {
            if (alphaStack.isEmpty()) return;
            s.append("Q\n");
            alpha = alphaStack.remove(alphaStack.size() - 1);
            matrix = matrices.remove(matrices.size() - 1);
        }

        private void ops(byte[] bytes) {
            ByteBuffer b = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
            while (b.hasRemaining()) {
                int op = b.get() & 0xff;
                switch (op) {
                    case DisplayList.SAVE -> push();
                    case DisplayList.RESTORE -> {
                        if (alphaStack.size() > floor) pop();
                    }
                    case DisplayList.TRANSLATE -> transform(new float[] {1, 0, 0, 1, b.getFloat(), b.getFloat()});
                    case DisplayList.SCALE -> {
                        float sx = b.getFloat(), sy = b.getFloat();
                        transform(new float[] {sx, 0, 0, sy, 0, 0});
                    }
                    case DisplayList.ROTATE -> {
                        double r = Math.toRadians(b.getFloat());
                        float c = (float) Math.cos(r), sn = (float) Math.sin(r);
                        transform(new float[] {c, sn, -sn, c, 0, 0});
                    }
                    case DisplayList.CONCAT -> transform(fromAndroid(floats(b, 9)));
                    case DisplayList.SET_MATRIX -> {
                        float[] to = fromAndroid(floats(b, 9));
                        cm(multiply(to, invert(matrix)));
                        matrix = to;
                    }
                    case DisplayList.CLIP_RECT -> {
                        float l = b.getFloat(), t = b.getFloat(), r = b.getFloat(), bt = b.getFloat();
                        if (b.get() == 0) clip(rect(l, t, r, bt), false); // a clip of difference does not clip
                    }
                    case DisplayList.CLIP_PATH -> {
                        int fill = b.get();
                        String p = path(b);
                        if (b.get() == 0) clip(fill >= 2 ? inverse(p) : p, fill == 1 || fill >= 2);
                    }
                    case DisplayList.DRAW_COLOR -> {
                        int color = b.getInt(), blend = b.get();
                        // CLEAR and SRC of transparent leave the page as it is: nothing under a page is drawn
                        if (blend != 0 && (color >>> 24) != 0) fill(color, everything(), false);
                    }
                    case DisplayList.DRAW_PAINT -> draw(everything(), false, paint(b), false);
                    case DisplayList.DRAW_RECT -> {
                        String r = rect(b.getFloat(), b.getFloat(), b.getFloat(), b.getFloat());
                        draw(r, false, paint(b), true);
                    }
                    case DisplayList.DRAW_RRECT -> {
                        String r = roundRect(b.getFloat(), b.getFloat(), b.getFloat(), b.getFloat(), b.getFloat(),
                                             b.getFloat());
                        draw(r, false, paint(b), true);
                    }
                    case DisplayList.DRAW_OVAL -> {
                        String o = oval(b.getFloat(), b.getFloat(), b.getFloat(), b.getFloat());
                        draw(o, false, paint(b), true);
                    }
                    case DisplayList.DRAW_CIRCLE -> {
                        float cx = b.getFloat(), cy = b.getFloat(), r = b.getFloat();
                        draw(oval(cx - r, cy - r, cx + r, cy + r), false, paint(b), true);
                    }
                    case DisplayList.DRAW_ARC -> {
                        float l = b.getFloat(), t = b.getFloat(), r = b.getFloat(), bt = b.getFloat();
                        float start = b.getFloat(), sweep = b.getFloat();
                        boolean center = b.get() != 0;
                        Paint p = paint(b);
                        draw(arc(l, t, r, bt, start, sweep, center, p.style != 1 || center), false, p, true);
                    }
                    case DisplayList.DRAW_LINES -> {
                        int count = b.getInt();
                        float[] v = floats(b, count);
                        StringBuilder lines = new StringBuilder();
                        for (int i = 0; i + 3 < count; i += 4)
                            lines.append(n(v[i])).append(' ').append(n(v[i + 1])).append(" m ").append(n(v[i + 2]))
                                .append(' ').append(n(v[i + 3])).append(" l\n");
                        Paint p = paint(b);
                        p.style = 1; // lines are stroked whatever the paint's style
                        draw(lines.toString(), false, p, true);
                    }
                    case DisplayList.DRAW_POINTS -> {
                        int count = b.getInt();
                        float[] v = floats(b, count);
                        Paint p = paint(b);
                        float half = Math.max(p.width, 1) / 2;
                        StringBuilder dots = new StringBuilder();
                        for (int i = 0; i + 1 < count; i += 2) {
                            float x = v[i], y = v[i + 1];
                            dots.append(p.cap == 1 ? oval(x - half, y - half, x + half, y + half)
                                                   : rect(x - half, y - half, x + half, y + half));
                        }
                        p.style = 0;
                        draw(dots.toString(), false, p, false);
                    }
                    case DisplayList.DRAW_PATH -> {
                        int fill = b.get();
                        String p = path(b);
                        draw(fill >= 2 ? inverse(p) : p, fill == 1 || fill >= 2, paint(b), true);
                    }
                    case DisplayList.DRAW_GLYPHS -> glyphs(b);
                    case DisplayList.DRAW_BITMAP -> bitmap(b);
                    case DisplayList.SAVE_LAYER -> {
                        floats(b, 4);
                        int layerAlpha = b.getInt();
                        b.get();
                        push();
                        alpha *= layerAlpha / 255f;
                    }
                    default -> throw new IllegalStateException(
                        "snx: PdfDocument: display list operation " + op + " at byte " + (b.position() - 1));
                }
            }
        }

        /** The canvas's transform concatenated: its tracked matrix too, for a later SET_MATRIX */
        private void transform(float[] m) {
            cm(m);
            matrix = multiply(m, matrix);
        }

        private void cm(float[] m) {
            for (float v : m) s.append(n(v)).append(' ');
            s.append("cm\n");
        }

        private void clip(String path, boolean evenOdd) {
            s.append(path).append(evenOdd ? "W* n\n" : "W n\n");
        }

        /** A rectangle over any page, in any transform: what DRAW_PAINT and DRAW_COLOR fill. PDF has no exponents */
        private String everything() { return "-100000 -100000 200000 200000 re\n"; }

        /** An inverse fill: the path and a rectangle over everything, even-odd */
        private String inverse(String path) { return everything() + path; }

        private void fill(int color, String path, boolean evenOdd) {
            state(color >>> 24, 255);
            s.append(rgb(color)).append(" rg\n").append(path).append(evenOdd ? "f*\n" : "f\n");
        }

        /** A shape with a paint: filled, stroked or both, as its style says; closed shapes close their strokes */
        private void draw(String path, boolean evenOdd, Paint p, boolean shape) {
            int a = (p.color >>> 24);
            if (a == 0 || p.blend == 0) return; // CLEAR, or nothing to draw: a page has nothing under it to erase
            String c = rgb(p.color);
            boolean fill = p.style != 1, stroke = p.style != 0;
            state(fill ? a : 255, stroke ? a : 255);
            if (stroke) {
                s.append(c).append(" RG ").append(n(p.width)).append(" w ").append(p.cap).append(" J ")
                    .append(p.join).append(" j ").append(n(Math.max(1, p.miter))).append(" M ");
                if (p.dash != null) {
                    s.append('[');
                    for (float d : p.dash) s.append(n(d)).append(' ');
                    s.append("] ").append(n(p.phase)).append(" d");
                } else {
                    s.append("[] 0 d");
                }
                s.append('\n');
            }
            if (fill) s.append(c).append(" rg\n");
            s.append(path);
            s.append(fill && stroke ? (evenOdd ? "B*\n" : "B\n") : fill ? (evenOdd ? "f*\n" : "f\n") : "S\n");
        }

        /** The graphics state of a fill and a stroke alpha, times the alpha of the nodes and layers */
        private void state(int fillAlpha, int strokeAlpha) {
            int f = Math.round(fillAlpha * alpha), k = Math.round(strokeAlpha * alpha);
            s.append("/GS").append(gs(f)).append(" gs /GS").append(gs(k)).append("s gs\n");
        }

        private int gs(int a) { return alphas.computeIfAbsent(a, x -> alphas.size() + 1); }

        private void glyphs(ByteBuffer b) {
            int font = b.getInt();
            float size = b.getFloat(), scaleX = b.getFloat(), skew = b.getFloat();
            boolean bold = b.get() != 0;
            int count = b.getInt();
            int[] ids = new int[count];
            for (int i = 0; i < count; ++i) ids[i] = b.getInt();
            float[] pos = floats(b, 2 * count);
            Paint p = paint(b);
            if ((p.color >>> 24) == 0 || count == 0) return;
            Font f = fonts.computeIfAbsent(font, h -> {
                Object[] file = Text.file(h);
                if (file == null) throw new IllegalStateException("snx: PdfDocument: font " + h + " has no file");
                return new Font((String) file[0], (Integer) file[1]);
            });
            int number = 1 + new ArrayList<>(fonts.keySet()).indexOf(font);
            state((p.color >>> 24), (p.color >>> 24));
            String c = rgb(p.color);
            s.append("BT\n/F").append(number).append(' ').append(n(size)).append(" Tf ").append(c).append(" rg ");
            if (bold) s.append(c).append(" RG ").append(n(size / 24)).append(" w 2 Tr\n");
            else s.append("0 Tr\n");
            for (int i = 0; i < count; ++i) {
                f.glyphs.set(ids[i]);
                s.append(n(scaleX)).append(" 0 ").append(n(-skew)).append(" -1 ").append(n(pos[2 * i])).append(' ')
                    .append(n(pos[2 * i + 1])).append(" Tm <").append(String.format(Locale.ROOT, "%04x", ids[i]))
                    .append("> Tj\n");
            }
            s.append("ET\n");
        }

        private void bitmap(ByteBuffer b) {
            int id = b.getInt();
            float sl = b.getFloat(), st = b.getFloat(), sr = b.getFloat(), sb = b.getFloat();
            float dl = b.getFloat(), dt = b.getFloat(), dr = b.getFloat(), db = b.getFloat();
            Paint p = paint(b);
            Bitmap bm = bitmaps.get(id);
            if (bm == null || sr <= sl || sb <= st) return;
            int[] pixels = bm.pixels();
            if (pixels == null) {
                if (bm.drawingOrNull() != null)
                    throw new UnsupportedOperationException("snx: PdfDocument: bitmap " + id + " (" + bm.getWidth()
                        + "x" + bm.getHeight() + ") was drawn by a canvas, whose pixels only the GPU has");
                return; // a bitmap nothing drew into is transparent
            }
            // a SrcIn or SrcAtop color filter gives its color where the bitmap is, as an icon's tint
            int tint = p.filter == 1 && (p.filterBlend == 5 || p.filterBlend == 9) ? p.filterColor : 0;
            int image = imageOf.computeIfAbsent(31 * id + tint, k -> {
                images.add(image(pixels, bm.getWidth(), bm.getHeight(), tint));
                return images.size();
            });
            float sx = (dr - dl) / (sr - sl), sy = (db - dt) / (sb - st);
            push();
            clip(rect(dl, dt, dr, db), false);
            state((tint != 0 ? tint >>> 24 : 255) * (p.color >>> 24) / 255, 255);
            float w = bm.getWidth() * sx, h = bm.getHeight() * sy;
            // the image's first row is the unit square's top, y = 1, which the page's y down puts at dt
            cm(new float[] {w, 0, 0, -h, dl - sl * sx, dt - st * sy + h});
            s.append("/Im").append(image).append(" Do\n");
            pop();
        }
    }

    /** The paint of an operation (docs/platforms/android-apps/hwui.md), as the PDF draws it */
    private static final class Paint {
        int color, style, cap, join, blend, filter, filterColor, filterBlend;
        float width, miter, phase;
        float[] dash;
    }

    private static Paint paint(ByteBuffer b) {
        Paint p = new Paint();
        p.color = b.getInt();
        p.style = b.get();
        p.cap = b.get();
        p.join = b.get();
        b.get(); // flags
        p.width = b.getFloat();
        p.miter = b.getFloat();
        p.blend = b.get();
        int shader = b.get();
        p.filter = b.get();
        int dashes = b.get() & 0xff;
        if (shader != 0) {
            b.get();
            b.get();
            int colors = b.getShort() & 0xffff;
            floats(b, 6 + 9);
            long r = 0, g = 0, bl = 0, a = 0;
            for (int i = 0; i < colors; ++i) {
                int c = b.getInt();
                a += c >>> 24;
                r += (c >> 16) & 0xff;
                g += (c >> 8) & 0xff;
                bl += c & 0xff;
            }
            boolean positions = b.get() != 0;
            b.get();
            b.get();
            b.get();
            if (positions) floats(b, colors);
            b.getInt(); // a bitmap shader's bitmap
            // a shader draws as its colors' average, as the renderer draws it
            if (colors > 0) {
                int alpha = (int) (a / colors) * (p.color >>> 24) / 255;
                p.color = alpha << 24 | (int) (r / colors) << 16 | (int) (g / colors) << 8 | (int) (bl / colors);
            }
        }
        if (p.filter == 1) {
            p.filterColor = b.getInt();
            p.filterBlend = b.get();
            b.get();
            b.get();
            b.get();
        } else if (p.filter == 2) {
            floats(b, 20);
        }
        if (dashes > 0) {
            p.phase = b.getFloat();
            p.dash = floats(b, dashes);
        }
        return p;
    }

    // ---- shapes as PDF paths ----

    private static String n(float v) {
        if (v == (long) v && Math.abs(v) < 1e9) return Long.toString((long) v);
        String t = String.format(Locale.ROOT, "%.4f", v);
        t = t.replaceAll("0+$", "");
        return t.endsWith(".") ? t.substring(0, t.length() - 1) : t;
    }

    private static String rgb(int color) {
        return n(((color >> 16) & 0xff) / 255f) + " " + n(((color >> 8) & 0xff) / 255f) + " "
            + n((color & 0xff) / 255f);
    }

    private static String rect(float l, float t, float r, float b) {
        return n(l) + " " + n(t) + " " + n(r - l) + " " + n(b - t) + " re\n";
    }

    private static String roundRect(float l, float t, float r, float b, float rx, float ry) {
        rx = Math.min(Math.abs(rx), (r - l) / 2);
        ry = Math.min(Math.abs(ry), (b - t) / 2);
        if (rx <= 0 || ry <= 0) return rect(l, t, r, b);
        float kx = rx * KAPPA, ky = ry * KAPPA;
        return n(l + rx) + " " + n(t) + " m\n" + n(r - rx) + " " + n(t) + " l\n"
            + curve(r - rx + kx, t, r, t + ry - ky, r, t + ry) + n(r) + " " + n(b - ry) + " l\n"
            + curve(r, b - ry + ky, r - rx + kx, b, r - rx, b) + n(l + rx) + " " + n(b) + " l\n"
            + curve(l + rx - kx, b, l, b - ry + ky, l, b - ry) + n(l) + " " + n(t + ry) + " l\n"
            + curve(l, t + ry - ky, l + rx - kx, t, l + rx, t) + "h\n";
    }

    private static String oval(float l, float t, float r, float b) {
        return roundRect(l, t, r, b, (r - l) / 2, (b - t) / 2);
    }

    private static String curve(float x1, float y1, float x2, float y2, float x3, float y3) {
        return n(x1) + " " + n(y1) + " " + n(x2) + " " + n(y2) + " " + n(x3) + " " + n(y3) + " c\n";
    }

    /** An arc of the oval from start through sweep degrees (clockwise, y down): to the center and closed, or not */
    private static String arc(float l, float t, float r, float b, float start, float sweep, boolean center,
                              boolean closed) {
        float cx = (l + r) / 2, cy = (t + b) / 2, rx = (r - l) / 2, ry = (b - t) / 2;
        sweep = Math.max(-360, Math.min(360, sweep));
        int parts = Math.max(1, (int) Math.ceil(Math.abs(sweep) / 90));
        double step = Math.toRadians(sweep) / parts, a = Math.toRadians(start);
        double k = 4.0 / 3 * Math.tan(step / 4);
        StringBuilder s = new StringBuilder();
        float x0 = cx + rx * (float) Math.cos(a), y0 = cy + ry * (float) Math.sin(a);
        if (center) s.append(n(cx)).append(' ').append(n(cy)).append(" m\n");
        s.append(n(x0)).append(' ').append(n(y0)).append(center ? " l\n" : " m\n");
        for (int i = 0; i < parts; ++i, a += step) {
            double c0 = Math.cos(a), s0 = Math.sin(a), c1 = Math.cos(a + step), s1 = Math.sin(a + step);
            s.append(curve(cx + rx * (float) (c0 - k * s0), cy + ry * (float) (s0 + k * c0),
                           cx + rx * (float) (c1 + k * s1), cy + ry * (float) (s1 - k * c1), cx + rx * (float) c1,
                           cy + ry * (float) s1));
        }
        if (closed) s.append("h\n");
        return s.toString();
    }

    /** A path of the display list (after its fill type): verbs, then their points */
    private static String path(ByteBuffer b) {
        int verbs = b.getInt(), count = b.getInt();
        byte[] v = new byte[verbs];
        b.get(v);
        float[] p = floats(b, count);
        StringBuilder s = new StringBuilder();
        int at = 0;
        float lastX = 0, lastY = 0;
        for (byte verb : v) {
            switch (verb) {
                case 0 -> s.append(n(lastX = p[at++])).append(' ').append(n(lastY = p[at++])).append(" m\n");
                case 1 -> s.append(n(lastX = p[at++])).append(' ').append(n(lastY = p[at++])).append(" l\n");
                case 2 -> {
                    // a quadratic as the cubic of the same curve
                    float qx = p[at++], qy = p[at++], x = p[at++], y = p[at++];
                    s.append(curve(lastX + 2f / 3 * (qx - lastX), lastY + 2f / 3 * (qy - lastY), x + 2f / 3 * (qx - x),
                                   y + 2f / 3 * (qy - y), x, y));
                    lastX = x;
                    lastY = y;
                }
                case 3 -> {
                    s.append(curve(p[at], p[at + 1], p[at + 2], p[at + 3], lastX = p[at + 4], lastY = p[at + 5]));
                    at += 6;
                }
                case 4 -> s.append("h\n");
                default -> throw new IllegalStateException("snx: PdfDocument: path verb " + verb);
            }
        }
        return s.toString();
    }

    /** A Path's bytes as a display list holds one: its fill type, verbs and points */
    private static byte[] pathBytes(Path path) {
        DisplayList.Writer w = new DisplayList.Writer();
        w.u8(path.getFillType().ordinal());
        w.i32(path.verbCount());
        w.i32(path.pointCount());
        w.bytes(path.verbs(), 0, path.verbCount());
        for (int i = 0; i < path.pointCount(); ++i) w.f32(path.points()[i]);
        return w.toArray();
    }

    private static float[] floats(ByteBuffer b, int count) {
        float[] f = new float[count];
        for (int i = 0; i < count; ++i) f[i] = b.getFloat();
        return f;
    }

    private static float[] values(Matrix m) {
        float[] v = new float[9];
        m.getValues(v);
        return v;
    }

    // ---- affine transforms as PDF writes them: x' = a x + c y + e, y' = b x + d y + f ----

    private static float[] identity() { return new float[] {1, 0, 0, 1, 0, 0}; }

    /** Matrix.getValues's order (scale x, skew x, translate x, skew y, scale y, translate y) as PDF's */
    private static float[] fromAndroid(float[] v) { return new float[] {v[0], v[3], v[1], v[4], v[2], v[5]}; }

    /** m applied first, then n */
    private static float[] multiply(float[] m, float[] n) {
        return new float[] {m[0] * n[0] + m[1] * n[2], m[0] * n[1] + m[1] * n[3], m[2] * n[0] + m[3] * n[2],
            m[2] * n[1] + m[3] * n[3], m[4] * n[0] + m[5] * n[2] + n[4], m[4] * n[1] + m[5] * n[3] + n[5]};
    }

    private static float[] invert(float[] m) {
        float det = m[0] * m[3] - m[1] * m[2];
        if (det == 0) return identity();
        return new float[] {m[3] / det, -m[1] / det, -m[2] / det, m[0] / det, (m[2] * m[5] - m[3] * m[4]) / det,
            (m[1] * m[4] - m[0] * m[5]) / det};
    }

    // ---- images ----

    private static Image image(int[] pixels, int width, int height, int tint) {
        byte[] rgb = new byte[width * height * 3], alpha = new byte[width * height];
        for (int i = 0; i < width * height; ++i) {
            int c = tint != 0 ? tint : pixels[i];
            rgb[3 * i] = (byte) (c >> 16);
            rgb[3 * i + 1] = (byte) (c >> 8);
            rgb[3 * i + 2] = (byte) c;
            alpha[i] = (byte) (pixels[i] >>> 24);
        }
        return new Image(width, height, rgb, alpha);
    }

    // ---- the file ----

    /** The document: its pages, then the fonts, images and graphics states they use */
    public void writeTo(OutputStream out) throws IOException {
        Objects o = new Objects();
        int catalog = o.reserve(), pagesRef = o.reserve();
        int[] fontRefs = new int[fonts.size()];
        for (int i = 0; i < fontRefs.length; ++i) fontRefs[i] = o.reserve();
        int[] imageRefs = new int[images.size()];
        for (int i = 0; i < imageRefs.length; ++i) imageRefs[i] = o.reserve();
        StringBuilder resources = new StringBuilder("<< /ProcSet [/PDF /Text /ImageB /ImageC]");
        if (!fonts.isEmpty()) {
            resources.append(" /Font <<");
            for (int i = 0; i < fontRefs.length; ++i)
                resources.append(" /F").append(i + 1).append(' ').append(fontRefs[i]).append(" 0 R");
            resources.append(" >>");
        }
        if (!images.isEmpty()) {
            resources.append(" /XObject <<");
            for (int i = 0; i < imageRefs.length; ++i)
                resources.append(" /Im").append(i + 1).append(' ').append(imageRefs[i]).append(" 0 R");
            resources.append(" >>");
        }
        if (!alphas.isEmpty()) {
            resources.append(" /ExtGState <<");
            for (Map.Entry<Integer, Integer> e : alphas.entrySet()) {
                String a = n(Math.max(0, Math.min(255, e.getKey())) / 255f);
                resources.append(" /GS").append(e.getValue()).append(" << /ca ").append(a).append(" >> /GS")
                    .append(e.getValue()).append("s << /CA ").append(a).append(" >>");
            }
            resources.append(" >>");
        }
        resources.append(" >>");
        StringBuilder kids = new StringBuilder();
        for (Page p : pages) {
            int content = o.stream("", p.content);
            int page = o.add("<< /Type /Page /Parent " + pagesRef + " 0 R /MediaBox [0 0 " + p.width + " " + p.height
                             + "] /Resources " + resources + " /Contents " + content + " 0 R >>");
            kids.append(page).append(" 0 R ");
        }
        o.set(catalog, "<< /Type /Catalog /Pages " + pagesRef + " 0 R >>");
        o.set(pagesRef, "<< /Type /Pages /Kids [" + kids + "] /Count " + pages.size() + " >>");
        int i = 0;
        for (Font f : fonts.values()) font(o, fontRefs[i++], f);
        for (i = 0; i < images.size(); ++i) {
            Image im = images.get(i);
            int mask = o.stream("/Type /XObject /Subtype /Image /Width " + im.width + " /Height " + im.height
                                + " /ColorSpace /DeviceGray /BitsPerComponent 8", im.alpha);
            o.setStream(imageRefs[i], "/Type /XObject /Subtype /Image /Width " + im.width + " /Height " + im.height
                        + " /ColorSpace /DeviceRGB /BitsPerComponent 8 /SMask " + mask + " 0 R", im.rgb);
        }
        o.writeTo(out, catalog);
    }

    /** A font as PDF's Type 0 over a TrueType CIDFont, glyph IDs as its CIDs, with the glyphs used and their text */
    private static void font(Objects o, int ref, Font f) throws IOException {
        byte[] file = java.nio.file.Files.readAllBytes(java.nio.file.Path.of(f.path));
        TrueType tt = new TrueType(file, f.index);
        byte[] subset = tt.subset(f.glyphs);
        float scale = 1000f / tt.unitsPerEm;
        int fontFile = o.stream("/Length1 " + subset.length, subset);
        String name = "SNXAAA+" + tt.name();
        int descriptor = o.add("<< /Type /FontDescriptor /FontName /" + name + " /Flags 4 /FontBBox ["
                               + n(tt.xMin * scale) + " " + n(tt.yMin * scale) + " " + n(tt.xMax * scale) + " "
                               + n(tt.yMax * scale) + "] /ItalicAngle 0 /Ascent " + n(tt.ascent * scale) + " /Descent "
                               + n(tt.descent * scale) + " /CapHeight " + n(tt.ascent * scale)
                               + " /StemV 80 /FontFile2 " + fontFile + " 0 R >>");
        StringBuilder widths = new StringBuilder();
        for (int g = f.glyphs.nextSetBit(0); g >= 0; g = f.glyphs.nextSetBit(g + 1))
            widths.append(g).append(" [").append(n(tt.advance(g) * scale)).append("] ");
        int cid = o.add("<< /Type /Font /Subtype /CIDFontType2 /BaseFont /" + name
                        + " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /FontDescriptor "
                        + descriptor + " 0 R /CIDToGIDMap /Identity /W [" + widths + "] >>");
        int toUnicode = o.stream("", tt.toUnicode(f.glyphs).getBytes(StandardCharsets.ISO_8859_1));
        o.set(ref, "<< /Type /Font /Subtype /Type0 /BaseFont /" + name + " /Encoding /Identity-H /DescendantFonts ["
                   + cid + " 0 R] /ToUnicode " + toUnicode + " 0 R >>");
    }

    /** The file's objects, numbered from 1, their streams compressed */
    private static final class Objects {
        private final ArrayList<byte[]> bodies = new ArrayList<>();

        int reserve() {
            bodies.add(null);
            return bodies.size();
        }

        void set(int ref, String body) { bodies.set(ref - 1, body.getBytes(StandardCharsets.ISO_8859_1)); }

        int add(String body) {
            int ref = reserve();
            set(ref, body);
            return ref;
        }

        int stream(String dictionary, byte[] data) {
            int ref = reserve();
            setStream(ref, dictionary, data);
            return ref;
        }

        void setStream(int ref, String dictionary, byte[] data) {
            Deflater d = new Deflater(Deflater.BEST_COMPRESSION);
            d.setInput(data);
            d.finish();
            ByteArrayOutputStream z = new ByteArrayOutputStream();
            byte[] buf = new byte[65536];
            while (!d.finished()) z.write(buf, 0, d.deflate(buf));
            d.end();
            ByteArrayOutputStream body = new ByteArrayOutputStream();
            byte[] head = ("<< " + dictionary + " /Filter /FlateDecode /Length " + z.size() + " >>\nstream\n")
                .getBytes(StandardCharsets.ISO_8859_1);
            body.write(head, 0, head.length);
            body.write(z.toByteArray(), 0, z.size());
            byte[] tail = "\nendstream".getBytes(StandardCharsets.ISO_8859_1);
            body.write(tail, 0, tail.length);
            bodies.set(ref - 1, body.toByteArray());
        }

        void writeTo(OutputStream out, int root) throws IOException {
            ByteArrayOutputStream f = new ByteArrayOutputStream();
            byte[] header = "%PDF-1.4\n%âãÏÓ\n".getBytes(StandardCharsets.ISO_8859_1);
            f.write(header, 0, header.length);
            long[] offsets = new long[bodies.size()];
            for (int i = 0; i < bodies.size(); ++i) {
                offsets[i] = f.size();
                byte[] head = ((i + 1) + " 0 obj\n").getBytes(StandardCharsets.ISO_8859_1);
                f.write(head, 0, head.length);
                f.write(bodies.get(i), 0, bodies.get(i).length);
                byte[] end = "\nendobj\n".getBytes(StandardCharsets.ISO_8859_1);
                f.write(end, 0, end.length);
            }
            long xref = f.size();
            StringBuilder t = new StringBuilder("xref\n0 ").append(bodies.size() + 1).append("\n0000000000 65535 f \n");
            for (long off : offsets) t.append(String.format(Locale.ROOT, "%010d 00000 n \n", off));
            t.append("trailer\n<< /Size ").append(bodies.size() + 1).append(" /Root ").append(root)
                .append(" 0 R >>\nstartxref\n").append(xref).append("\n%%EOF\n");
            byte[] tail = t.toString().getBytes(StandardCharsets.ISO_8859_1);
            f.write(tail, 0, tail.length);
            f.writeTo(out);
        }
    }
}
