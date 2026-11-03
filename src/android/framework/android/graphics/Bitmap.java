// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

import java.nio.Buffer;
import java.nio.ByteBuffer;
import java.nio.IntBuffer;
import java.util.concurrent.atomic.AtomicInteger;
import snx.android.hwui.DisplayList;

/**
 * Pixels in host memory, ARGB as ints (unpremultiplied, as getPixel returns them). The renderer uploads a bitmap
 * when a frame draws it and its generation changed. Drawing into it with a Canvas records a display list.
 */
public final class Bitmap implements android.os.Parcelable {
    public static final int DENSITY_NONE = 0;

    public enum Config { ALPHA_8, RGB_565, ARGB_4444, ARGB_8888, RGBA_F16, HARDWARE, RGBA_1010102 }
    public enum CompressFormat { JPEG, PNG, WEBP, WEBP_LOSSY, WEBP_LOSSLESS }

    private static final AtomicInteger IDS = new AtomicInteger(1);
    private final int id = IDS.getAndIncrement();
    private int width, height;
    private int[] pixels;
    private Config config;
    private boolean hasAlpha = true, mutable = true, recycled, premultiplied = true;
    private int density = android.util.DisplayMetrics.DENSITY_DEVICE_STABLE;
    private int generation;
    private DisplayList drawing;
    /** Pixels the app set that the renderer has not taken yet (its image holds what was drawn) */
    private boolean pixelsDirty;
    /** The bitmaps that the app no longer holds, whose images the renderer frees */
    private static final java.util.concurrent.ConcurrentLinkedQueue<Integer> FREED = new java.util.concurrent.ConcurrentLinkedQueue<>();
    private static final java.lang.ref.Cleaner CLEANER = java.lang.ref.Cleaner.create();
    private ColorSpace colorSpace = ColorSpace.get(ColorSpace.Named.SRGB);
    /** The nine-patch chunk of a decoded image, or null */
    byte[] ninePatchChunk;

    private Bitmap(int width, int height, Config config) {
        if (width <= 0 || height <= 0) throw new IllegalArgumentException("width and height must be > 0");
        this.width = width;
        this.height = height;
        this.config = config != null ? config : Config.ARGB_8888;
        pixels = new int[width * height];
        int freedId = id;
        CLEANER.register(this, () -> FREED.add(freedId));
    }

    /** The IDs of the bitmaps gone since the last call */
    public static int[] takeFreed() {
        java.util.ArrayList<Integer> ids = new java.util.ArrayList<>();
        for (Integer i; (i = FREED.poll()) != null;) ids.add(i);
        int[] out = new int[ids.size()];
        for (int k = 0; k < out.length; ++k) out[k] = ids.get(k);
        return out;
    }
    private void markPixels() {
        pixelsDirty = true;
        ++generation;
    }
    /** The pixels to upload (all of them), or null when the app set none since the last call */
    public int[] takePixels() {
        if (!pixelsDirty) return null;
        pixelsDirty = false;
        return pixels;
    }

    /** The bitmap's number for the renderer, and its pixels' generation. */
    public int id() { return id; }
    public int generation() { return generation; }
    public int[] pixels() { return pixels; }
    /** What canvases drew into the bitmap since the renderer last took it, or null */
    public DisplayList drawingOrNull() { return drawing; }
    DisplayList drawing() {
        if (drawing == null) drawing = new DisplayList();
        ++generation;
        return drawing;
    }

    public static Bitmap createBitmap(int width, int height, Config config) { return new Bitmap(width, height, config); }
    public static Bitmap createBitmap(int width, int height, Config config, boolean hasAlpha) {
        Bitmap b = new Bitmap(width, height, config);
        b.hasAlpha = hasAlpha;
        return b;
    }
    public static Bitmap createBitmap(int width, int height, Config config, boolean hasAlpha, ColorSpace colorSpace) {
        Bitmap b = createBitmap(width, height, config, hasAlpha);
        if (colorSpace != null) b.colorSpace = colorSpace;
        return b;
    }
    public static Bitmap createBitmap(android.util.DisplayMetrics display, int width, int height, Config config) { return createBitmap(width, height, config); }
    public static Bitmap createBitmap(android.util.DisplayMetrics display, int width, int height, Config config, boolean hasAlpha, ColorSpace colorSpace) {
        return createBitmap(width, height, config, hasAlpha, colorSpace);
    }
    public static Bitmap createBitmap(int[] colors, int width, int height, Config config) { return createBitmap(colors, 0, width, width, height, config); }
    public static Bitmap createBitmap(int[] colors, int offset, int stride, int width, int height, Config config) {
        Bitmap b = new Bitmap(width, height, config);
        b.setPixels(colors, offset, stride, 0, 0, width, height);
        b.mutable = false;
        return b;
    }
    public static Bitmap createBitmap(Bitmap src) { return createBitmap(src, 0, 0, src.width, src.height); }
    public static Bitmap createBitmap(Bitmap src, int x, int y, int width, int height) {
        return createBitmap(src, x, y, width, height, null, false);
    }
    /** A part of src through a matrix, drawn on the GPU as Android's Skia draws it */
    public static Bitmap createBitmap(Bitmap src, int x, int y, int width, int height, Matrix m, boolean filter) {
        RectF bounds = new RectF(0, 0, width, height);
        if (m != null) m.mapRect(bounds);
        int w = Math.max(1, Math.round(bounds.width())), h = Math.max(1, Math.round(bounds.height()));
        Bitmap b = new Bitmap(w, h, src.config);
        b.hasAlpha = src.hasAlpha;
        Canvas c = new Canvas(b);
        c.translate(-bounds.left, -bounds.top);
        if (m != null) c.concat(m);
        c.drawBitmap(src, new Rect(x, y, x + width, y + height), new RectF(0, 0, width, height), filter ? new Paint(Paint.FILTER_BITMAP_FLAG) : null);
        return b;
    }
    public static Bitmap createScaledBitmap(Bitmap src, int dstWidth, int dstHeight, boolean filter) {
        Bitmap b = new Bitmap(dstWidth, dstHeight, src.config);
        b.hasAlpha = src.hasAlpha;
        new Canvas(b).drawBitmap(src, null, new RectF(0, 0, dstWidth, dstHeight), filter ? new Paint(Paint.FILTER_BITMAP_FLAG) : null);
        return b;
    }
    public static Bitmap createBitmap(Picture source) { return createBitmap(source, source.getWidth(), source.getHeight(), Config.ARGB_8888); }
    public static Bitmap createBitmap(Picture source, int width, int height, Config config) {
        Bitmap b = new Bitmap(Math.max(1, width), Math.max(1, height), config);
        new Canvas(b).drawPicture(source);
        return b;
    }

    public int getWidth() { return width; }
    public int getHeight() { return height; }
    public int getScaledWidth(int targetDensity) { return density == 0 || targetDensity == 0 ? width : (width * targetDensity + (density >> 1)) / density; }
    public int getScaledHeight(int targetDensity) { return density == 0 || targetDensity == 0 ? height : (height * targetDensity + (density >> 1)) / density; }
    public int getScaledWidth(Canvas canvas) { return getScaledWidth(canvas.getDensity()); }
    public int getScaledHeight(Canvas canvas) { return getScaledHeight(canvas.getDensity()); }
    public int getDensity() { return density; }
    public void setDensity(int density) { this.density = density; }
    public final Config getConfig() { return config; }
    public int getRowBytes() { return width * 4; }
    public final int getByteCount() { return width * height * 4; }
    public final int getAllocationByteCount() { return getByteCount(); }
    public final boolean hasAlpha() { return hasAlpha; }
    public void setHasAlpha(boolean hasAlpha) { this.hasAlpha = hasAlpha; }
    public final boolean hasMipMap() { return false; }
    public final void setHasMipMap(boolean hasMipMap) {}
    public final boolean isMutable() { return mutable; }
    public final boolean isPremultiplied() { return premultiplied; }
    public final void setPremultiplied(boolean premultiplied) { this.premultiplied = premultiplied; }
    public byte[] getNinePatchChunk() { return ninePatchChunk; }
    /** No bitmap here has a hardware buffer behind it: the pixels stay in host memory. */
    public android.hardware.HardwareBuffer getHardwareBuffer() {
        if (config != Config.HARDWARE || recycled) throw new IllegalStateException("Bitmap is not a HARDWARE bitmap");
        return null;
    }
    public final ColorSpace getColorSpace() { return colorSpace; }
    public void setColorSpace(ColorSpace colorSpace) { this.colorSpace = colorSpace; }
    public void recycle() { recycled = true; }
    public final boolean isRecycled() { return recycled; }
    public int getGenerationId() { return generation; }
    public void prepareToDraw() {}
    public void setWidth(int width) { reconfigure(width, height, config); }
    public void setHeight(int height) { reconfigure(width, height, config); }
    public void setConfig(Config config) { this.config = config; }
    public void reconfigure(int width, int height, Config config) {
        this.width = width;
        this.height = height;
        this.config = config;
        if (pixels.length < width * height) pixels = new int[width * height];
        markPixels();
    }

    public int getPixel(int x, int y) { return pixels[y * width + x]; }
    public Color getColor(int x, int y) { return Color.valueOf(getPixel(x, y)); }
    public void setPixel(int x, int y, int color) { pixels[y * width + x] = color; markPixels(); }
    public void getPixels(int[] out, int offset, int stride, int x, int y, int width, int height) {
        for (int row = 0; row < height; ++row) System.arraycopy(pixels, (y + row) * this.width + x, out, offset + row * stride, width);
    }
    public void setPixels(int[] in, int offset, int stride, int x, int y, int width, int height) {
        for (int row = 0; row < height; ++row) System.arraycopy(in, offset + row * stride, pixels, (y + row) * this.width + x, width);
        markPixels();
    }
    public void eraseColor(int c) {
        java.util.Arrays.fill(pixels, c);
        if (drawing != null) drawing.clear(); /* an open canvas keeps writing to it */
        markPixels();
    }
    public void eraseColor(long color) { eraseColor(Color.toArgb(color)); }

    /** The pixels as RGBA bytes (the order ARGB_8888 has in memory on Android). */
    public void copyPixelsToBuffer(Buffer dst) {
        if (dst instanceof IntBuffer ib) {
            for (int p : pixels) ib.put(p);
            return;
        }
        ByteBuffer b = (ByteBuffer) dst;
        for (int p : pixels) {
            b.put((byte) (p >> 16)); b.put((byte) (p >> 8)); b.put((byte) p); b.put((byte) (p >>> 24));
        }
    }
    public void copyPixelsFromBuffer(Buffer src) {
        if (src instanceof IntBuffer ib) {
            ib.get(pixels, 0, Math.min(pixels.length, ib.remaining()));
        } else {
            ByteBuffer b = (ByteBuffer) src;
            for (int i = 0; i < pixels.length && b.remaining() >= 4; ++i) {
                int r = b.get() & 0xff, g = b.get() & 0xff, bl = b.get() & 0xff, a = b.get() & 0xff;
                pixels[i] = a << 24 | r << 16 | g << 8 | bl;
            }
        }
        markPixels();
    }
    public Bitmap copy(Config config, boolean isMutable) {
        Bitmap b = createBitmap(this, 0, 0, width, height, null, false);
        b.config = config;
        b.mutable = isMutable;
        return b;
    }
    public Bitmap extractAlpha() {
        Bitmap b = new Bitmap(width, height, Config.ALPHA_8);
        for (int i = 0; i < pixels.length; ++i) b.pixels[i] = pixels[i] & 0xff000000;
        b.markPixels();
        return b;
    }
    public boolean sameAs(Bitmap other) {
        return other != null && other.width == width && other.height == height && java.util.Arrays.equals(other.pixels, pixels);
    }
    public boolean compress(CompressFormat format, int quality, java.io.OutputStream stream) {
        try {
            java.awt.image.BufferedImage image = new java.awt.image.BufferedImage(width, height, java.awt.image.BufferedImage.TYPE_INT_ARGB);
            image.setRGB(0, 0, width, height, pixels, 0, width);
            return javax.imageio.ImageIO.write(image, format == CompressFormat.JPEG ? "jpg" : "png", stream);
        } catch (java.io.IOException e) {
            return false;
        }
    }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel p, int flags) { p.writeValue(this); }
}
