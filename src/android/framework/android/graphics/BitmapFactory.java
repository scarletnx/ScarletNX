// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

import android.content.res.Resources;
import android.util.DisplayMetrics;
import android.util.TypedValue;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import snx.android.hwui.Image;

/**
 * Bitmaps from encoded images (PNG, JPEG, WebP, GIF, BMP), decoded by the runtime's codec (snx.android.hwui.Image).
 * A PNG's nine-patch chunk (npTc, which aapt writes) becomes the bitmap's, and its padding the decode's.
 */
public class BitmapFactory {
    public static class Options {
        public Bitmap inBitmap;
        public boolean inMutable;
        public boolean inJustDecodeBounds;
        public int inSampleSize;
        public Bitmap.Config inPreferredConfig = Bitmap.Config.ARGB_8888;
        public ColorSpace inPreferredColorSpace;
        public boolean inPremultiplied = true;
        @Deprecated public boolean inDither;
        public int inDensity;
        public int inTargetDensity;
        public int inScreenDensity;
        public boolean inScaled = true;
        @Deprecated public boolean inPurgeable;
        @Deprecated public boolean inInputShareable;
        @Deprecated public boolean inPreferQualityOverSpeed;
        public int outWidth;
        public int outHeight;
        public String outMimeType;
        public Bitmap.Config outConfig;
        public ColorSpace outColorSpace;
        public byte[] inTempStorage;
        @Deprecated public boolean mCancel;

        public Options() {}
        /** Ignored since Android 7, as on Android. */
        @Deprecated public void requestCancelDecode() {}
    }

    public BitmapFactory() {}

    public static Bitmap decodeFile(String pathName) { return decodeFile(pathName, null); }
    public static Bitmap decodeFile(String pathName, Options opts) {
        try (InputStream in = new FileInputStream(pathName)) {
            return decodeStream(in, null, opts);
        } catch (IOException e) {
            return null;
        }
    }
    public static Bitmap decodeResource(Resources res, int id) { return decodeResource(res, id, null); }
    public static Bitmap decodeResource(Resources res, int id, Options opts) {
        TypedValue value = new TypedValue();
        try (InputStream in = res.openRawResource(id, value)) {
            return decodeResourceStream(res, value, in, null, opts);
        } catch (IOException | RuntimeException e) {
            return null;
        }
    }
    public static Bitmap decodeResourceStream(Resources res, TypedValue value, InputStream is, Rect pad, Options opts) {
        if (opts == null) opts = new Options();
        if (opts.inDensity == 0 && value != null) {
            int density = value.density;
            opts.inDensity = density == TypedValue.DENSITY_DEFAULT ? DisplayMetrics.DENSITY_DEFAULT : density == TypedValue.DENSITY_NONE ? 0 : density;
        }
        if (opts.inTargetDensity == 0 && res != null) opts.inTargetDensity = res.getDisplayMetrics().densityDpi;
        return decodeStream(is, pad, opts);
    }
    public static Bitmap decodeByteArray(byte[] data, int offset, int length) { return decodeByteArray(data, offset, length, null); }
    public static Bitmap decodeByteArray(byte[] data, int offset, int length, Options opts) {
        if ((offset | length) < 0 || data.length - offset < length) throw new ArrayIndexOutOfBoundsException();
        return decode(data, offset, length, null, opts);
    }
    public static Bitmap decodeFileDescriptor(FileDescriptor fd) { return decodeFileDescriptor(fd, null, null); }
    public static Bitmap decodeFileDescriptor(FileDescriptor fd, Rect outPadding, Options opts) {
        return decodeStream(new FileInputStream(fd), outPadding, opts); /* the caller keeps the descriptor open */
    }
    public static Bitmap decodeStream(InputStream is) { return decodeStream(is, null, null); }
    public static Bitmap decodeStream(InputStream is, Rect outPadding, Options opts) {
        byte[] data;
        try {
            data = is != null ? is.readAllBytes() : null;
        } catch (IOException e) {
            data = null;
        }
        if (data == null) {
            if (outPadding != null) outPadding.set(-1, -1, -1, -1);
            if (opts != null) {
                opts.outWidth = opts.outHeight = -1;
                opts.outMimeType = null;
            }
            return null;
        }
        return decode(data, 0, data.length, outPadding, opts);
    }

    /** The bitmap of data[offset, offset + length): Android's doDecode, with the sample size, the density scale and
     *  the nine-patch chunk */
    private static Bitmap decode(byte[] data, int offset, int length, Rect outPadding, Options opts) {
        if (outPadding != null) outPadding.set(-1, -1, -1, -1);
        if (opts != null) {
            opts.outWidth = opts.outHeight = -1;
            opts.outMimeType = null;
        }
        int sample = opts != null && opts.inSampleSize > 1 ? opts.inSampleSize : 1;
        boolean boundsOnly = opts != null && opts.inJustDecodeBounds;
        Bitmap.Config config = opts != null && opts.inPreferredConfig != null ? opts.inPreferredConfig : Bitmap.Config.ARGB_8888;
        Bitmap reuse = opts != null && !boundsOnly ? opts.inBitmap : null;
        /* the scale of a density: not for a bitmap to reuse, and not for bounds alone (Android gives a scaled size
           with inJustDecodeBounds too) */
        float scale = 1f;
        if (opts != null && opts.inScaled && opts.inDensity != 0 && opts.inTargetDensity != 0
                && opts.inDensity != opts.inScreenDensity && reuse == null)
            scale = opts.inTargetDensity / (float) opts.inDensity;
        int[] info = new int[4];
        int[] pixels = Image.decode(data, offset, length, boundsOnly ? 0 : sample, scale, info);
        if (info[0] <= 0) return null;
        byte[] chunk = info[2] == Image.PNG ? ninePatchChunk(data, offset, length) : null;
        if (opts != null) {
            opts.outWidth = info[0];
            opts.outHeight = info[1];
            opts.outMimeType = Image.mimeType(info[2]);
            opts.outConfig = config;
            opts.outColorSpace = ColorSpace.get(ColorSpace.Named.SRGB);
        }
        if (chunk != null && scale != 1f) scaleNinePatch(chunk, scale, info[0], info[1]);
        if (outPadding != null && chunk != null) {
            outPadding.set(chunk32(chunk, 12), chunk32(chunk, 20), chunk32(chunk, 16), chunk32(chunk, 24));
        }
        if (boundsOnly || pixels == null) return null;
        int w = info[0], h = info[1];
        if (config == Bitmap.Config.ALPHA_8) for (int i = 0; i < pixels.length; ++i) pixels[i] &= 0xff000000;
        Bitmap b;
        if (reuse != null && reuse.isMutable() && !reuse.isRecycled()) {
            b = reuse;
            b.reconfigure(w, h, config);
            b.setPixels(pixels, 0, w, 0, 0, w, h);
        } else {
            b = Bitmap.ofPixels(pixels, w, h, config, opts != null && opts.inMutable);
        }
        b.setHasAlpha(info[3] != 0 || config == Bitmap.Config.ALPHA_8);
        b.ninePatchChunk = chunk;
        if (opts != null && opts.inDensity != 0) b.setDensity(scale != 1f ? opts.inTargetDensity : opts.inDensity);
        return b;
    }

    /** The npTc chunk of a PNG, in a device's (little-endian) order as Android's NinePatchPeeker makes it, or null */
    private static byte[] ninePatchChunk(byte[] data, int offset, int length) {
        int end = offset + length;
        for (int at = offset + 8; end - at >= 12;) {
            int size = be32(data, at);
            if (size < 0 || end - at - 12 < size) return null;
            int type = be32(data, at + 4);
            if (type == 0x6e705463 /* npTc */) {
                if (size < 32) return null;
                int nx = data[at + 9] & 0xff, ny = data[at + 10] & 0xff, nc = data[at + 11] & 0xff;
                if (size < 32 + 4 * (nx + ny + nc)) return null;
                byte[] chunk = new byte[size];
                System.arraycopy(data, at + 8, chunk, 0, size);
                /* every field from byte 4 on is 32 bits: the offsets, the padding, the divs and the colors */
                for (int i = 4; i + 4 <= 32 + 4 * (nx + ny + nc); i += 4) {
                    byte b0 = chunk[i], b1 = chunk[i + 1];
                    chunk[i] = chunk[i + 3];
                    chunk[i + 1] = chunk[i + 2];
                    chunk[i + 2] = b1;
                    chunk[i + 3] = b0;
                }
                return chunk;
            }
            if (type == 0x49444154 /* IDAT */ || type == 0x49454e44 /* IEND */) return null;
            at += 12 + size;
        }
        return null;
    }
    private static int be32(byte[] b, int at) {
        return (b[at] & 0xff) << 24 | (b[at + 1] & 0xff) << 16 | (b[at + 2] & 0xff) << 8 | b[at + 3] & 0xff;
    }
    /** A 32-bit field of a chunk in device order */
    static int chunk32(byte[] c, int at) {
        return c[at] & 0xff | (c[at + 1] & 0xff) << 8 | (c[at + 2] & 0xff) << 16 | (c[at + 3] & 0xff) << 24;
    }
    private static void putChunk32(byte[] c, int at, int v) {
        c[at] = (byte) v;
        c[at + 1] = (byte) (v >> 8);
        c[at + 2] = (byte) (v >> 16);
        c[at + 3] = (byte) (v >> 24);
    }
    /** The chunk of a bitmap scaled by scale, as Android's scaleNinePatchChunk: the padding and the divs, each div
     *  at least one pixel after the one before it and inside the bitmap */
    private static void scaleNinePatch(byte[] chunk, float scale, int width, int height) {
        for (int at = 12; at < 28; at += 4) putChunk32(chunk, at, (int) (chunk32(chunk, at) * scale + 0.5f));
        int nx = chunk[1] & 0xff, ny = chunk[2] & 0xff;
        scaleDivs(chunk, 32, nx, scale, width);
        scaleDivs(chunk, 32 + 4 * nx, ny, scale, height);
    }
    private static void scaleDivs(byte[] chunk, int at, int count, float scale, int max) {
        int highest = 0;
        for (int i = 0; i < count; ++i) {
            int div = (int) (chunk32(chunk, at + 4 * i) * scale + 0.5f);
            if (i > 0 && div <= highest) div = highest + 1;
            highest = Math.min(div, max);
            putChunk32(chunk, at + 4 * i, highest);
        }
    }
}
