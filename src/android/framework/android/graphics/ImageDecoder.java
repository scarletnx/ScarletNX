// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

import android.content.ContentResolver;
import android.content.res.Resources;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.Drawable;
import android.net.Uri;
import android.util.Size;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;

/** Decodes images into bitmaps and drawables, through BitmapFactory and the runtime's codec. */
public final class ImageDecoder implements AutoCloseable {
    public static final int ALLOCATOR_DEFAULT = 0, ALLOCATOR_SOFTWARE = 1, ALLOCATOR_SHARED_MEMORY = 2, ALLOCATOR_HARDWARE = 3;
    public static final int MEMORY_POLICY_DEFAULT = 1, MEMORY_POLICY_LOW_RAM = 0;

    public abstract static class Source {
        Source() {}
        abstract InputStream open() throws IOException;
        Resources resources() { return null; }
    }

    public static class ImageInfo {
        private final Size size;
        private final String mimeType;

        ImageInfo(Size size, String mimeType) { this.size = size; this.mimeType = mimeType; }
        public Size getSize() { return size; }
        public String getMimeType() { return mimeType; }
        public boolean isAnimated() { return false; }
        public ColorSpace getColorSpace() { return ColorSpace.get(ColorSpace.Named.SRGB); }
    }

    public interface OnHeaderDecodedListener {
        void onHeaderDecoded(ImageDecoder decoder, ImageInfo info, Source source);
    }

    private int targetWidth, targetHeight, sampleSize = 1, allocator = ALLOCATOR_DEFAULT;
    private boolean mutable;

    private ImageDecoder() {}

    public static Source createSource(ContentResolver cr, Uri uri) {
        return new Source() { @Override InputStream open() throws IOException { return cr.openInputStream(uri); } };
    }
    public static Source createSource(Resources res, int resId) {
        return new Source() {
            @Override InputStream open() { return res.openRawResource(resId); }
            @Override Resources resources() { return res; }
        };
    }
    public static Source createSource(File file) {
        return new Source() { @Override InputStream open() throws IOException { return new java.io.FileInputStream(file); } };
    }
    public static Source createSource(byte[] data) { return createSource(data, 0, data.length); }
    public static Source createSource(byte[] data, int offset, int length) {
        return new Source() { @Override InputStream open() { return new java.io.ByteArrayInputStream(data, offset, length); } };
    }

    public void setTargetSize(int width, int height) {
        if (width <= 0 || height <= 0) throw new IllegalArgumentException("Dimensions must be positive! provided (" + width + ", " + height + ")");
        targetWidth = width;
        targetHeight = height;
    }
    public void setTargetSampleSize(int sampleSize) { this.sampleSize = Math.max(1, sampleSize); }
    public void setAllocator(int allocator) { this.allocator = allocator; }
    public int getAllocator() { return allocator; }
    public void setMutableRequired(boolean mutable) { this.mutable = mutable; }
    public boolean isMutableRequired() { return mutable; }
    @Override public void close() {}

    public static Bitmap decodeBitmap(Source src) throws IOException { return decodeBitmap(src, null); }
    public static Bitmap decodeBitmap(Source src, OnHeaderDecodedListener listener) throws IOException {
        byte[] data;
        try (InputStream in = src.open()) {
            if (in == null) throw new IOException("ImageDecoder: no data in the source");
            data = in.readAllBytes();
        }
        BitmapFactory.Options opts = new BitmapFactory.Options();
        opts.inJustDecodeBounds = true;
        BitmapFactory.decodeByteArray(data, 0, data.length, opts);
        if (opts.outWidth <= 0) throw new IOException("ImageDecoder: the image format is not supported");
        ImageDecoder decoder = new ImageDecoder();
        if (listener != null) listener.onHeaderDecoded(decoder, new ImageInfo(new Size(opts.outWidth, opts.outHeight), opts.outMimeType), src);
        int width = opts.outWidth, height = opts.outHeight;
        opts.inJustDecodeBounds = false;
        opts.inSampleSize = decoder.sampleSize;
        opts.inMutable = decoder.mutable;
        opts.inScaled = false;
        Bitmap b = BitmapFactory.decodeByteArray(data, 0, data.length, opts);
        if (b == null) throw new IOException("ImageDecoder: the image could not be decoded");
        if (decoder.targetWidth > 0 && (b.getWidth() != decoder.targetWidth || b.getHeight() != decoder.targetHeight))
            b = Bitmap.createScaledBitmap(b, decoder.targetWidth, decoder.targetHeight, true);
        else if (decoder.targetWidth == 0 && decoder.sampleSize > 1)
            b = Bitmap.createScaledBitmap(b, Math.max(1, width / decoder.sampleSize), Math.max(1, height / decoder.sampleSize), true);
        return b;
    }
    public static Drawable decodeDrawable(Source src) throws IOException { return decodeDrawable(src, null); }
    public static Drawable decodeDrawable(Source src, OnHeaderDecodedListener listener) throws IOException {
        return new BitmapDrawable(src.resources(), decodeBitmap(src, listener));
    }
}
