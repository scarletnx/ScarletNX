// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

import android.content.res.Resources;
import android.util.DisplayMetrics;
import android.util.TypedValue;
import java.io.ByteArrayInputStream;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Iterator;
import javax.imageio.ImageIO;
import javax.imageio.ImageReadParam;
import javax.imageio.ImageReader;
import javax.imageio.stream.ImageInputStream;

/** Bitmaps from encoded images, decoded with the host's image readers (PNG, JPEG, GIF, BMP). */
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
        if ((offset | length) < 0 || data.length < offset + length) throw new ArrayIndexOutOfBoundsException();
        return decodeStream(new ByteArrayInputStream(data, offset, length), null, opts);
    }
    public static Bitmap decodeFileDescriptor(FileDescriptor fd) { return decodeFileDescriptor(fd, null, null); }
    public static Bitmap decodeFileDescriptor(FileDescriptor fd, Rect outPadding, Options opts) {
        return decodeStream(new FileInputStream(fd), outPadding, opts); /* the caller keeps the descriptor open */
    }
    public static Bitmap decodeStream(InputStream is) { return decodeStream(is, null, null); }
    public static Bitmap decodeStream(InputStream is, Rect outPadding, Options opts) {
        if (outPadding != null) outPadding.set(-1, -1, -1, -1);
        if (opts != null) {
            opts.outWidth = opts.outHeight = -1;
            opts.outMimeType = null;
        }
        if (is == null) return null;
        try (ImageInputStream iis = ImageIO.createImageInputStream(is)) {
            Iterator<ImageReader> readers = iis != null ? ImageIO.getImageReaders(iis) : null;
            if (readers == null || !readers.hasNext()) return null;
            ImageReader reader = readers.next();
            try {
                reader.setInput(iis, true);
                return decode(reader, opts);
            } finally {
                reader.dispose();
            }
        } catch (IOException | RuntimeException e) {
            return null;
        }
    }

    private static Bitmap decode(ImageReader reader, Options opts) throws IOException {
        int sample = opts != null && opts.inSampleSize > 1 ? Integer.highestOneBit(opts.inSampleSize) : 1;
        int w = (reader.getWidth(0) + sample - 1) / sample, h = (reader.getHeight(0) + sample - 1) / sample;
        Bitmap.Config config = opts != null && opts.inPreferredConfig != null ? opts.inPreferredConfig : Bitmap.Config.ARGB_8888;
        if (opts != null) {
            opts.outWidth = w;
            opts.outHeight = h;
            String[] mimes = reader.getOriginatingProvider().getMIMETypes();
            opts.outMimeType = mimes != null && mimes.length > 0 ? mimes[0] : null;
            opts.outConfig = config;
            opts.outColorSpace = ColorSpace.get(ColorSpace.Named.SRGB);
            if (opts.inJustDecodeBounds) return null;
        }
        ImageReadParam param = reader.getDefaultReadParam();
        param.setSourceSubsampling(sample, sample, 0, 0);
        java.awt.image.BufferedImage image = reader.read(0, param);
        w = image.getWidth();
        h = image.getHeight();
        int[] pixels = image.getRGB(0, 0, w, h, null, 0, w);
        Bitmap reuse = opts != null ? opts.inBitmap : null;
        Bitmap b;
        if (reuse != null && reuse.isMutable() && !reuse.isRecycled()) {
            b = reuse;
            b.reconfigure(w, h, config);
            b.setPixels(pixels, 0, w, 0, 0, w, h);
        } else if (opts != null && opts.inMutable) {
            b = Bitmap.createBitmap(w, h, config);
            b.setPixels(pixels, 0, w, 0, 0, w, h);
        } else {
            b = Bitmap.createBitmap(pixels, w, h, config);
        }
        b.setHasAlpha(image.getColorModel().hasAlpha());
        if (opts == null || opts.inDensity == 0) return b;
        int density = opts.inDensity, target = opts.inTargetDensity;
        if (opts.inScaled && target != 0 && target != density && b != reuse) {
            float scale = target / (float) density;
            Bitmap scaled = Bitmap.createScaledBitmap(b, Math.max(1, (int) (w * scale + 0.5f)), Math.max(1, (int) (h * scale + 0.5f)), true);
            scaled.setDensity(target);
            opts.outWidth = scaled.getWidth();
            opts.outHeight = scaled.getHeight();
            return scaled;
        }
        b.setDensity(density);
        return b;
    }
}
