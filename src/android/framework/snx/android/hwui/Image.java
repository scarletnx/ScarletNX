// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.hwui;

/**
 * The image codec of BitmapFactory and Bitmap.compress, the HLE of Skia's codecs over FFmpeg
 * (src/android/hwui/image.c). Pixels are Android's int colors, 0xAARRGGBB, not premultiplied.
 */
public final class Image {
    private Image() {}

    /** The formats, as info[2] gives them and encode takes them. */
    public static final int PNG = 1, JPEG = 2, WEBP = 3, GIF = 4, BMP = 5;
    private static final String[] MIME_TYPES = {null, "image/png", "image/jpeg", "image/webp", "image/gif", "image/bmp"};

    /** The MIME type of a format, null for none. */
    public static String mimeType(int format) { return format > 0 && format < MIME_TYPES.length ? MIME_TYPES[format] : null; }

    /**
     * The pixels of the image in data[offset, offset + length), its size divided by sample (rounded up), then scaled
     * by scale (rounded to the nearest pixel), as Skia's sampled decode and BitmapFactory's density scale give it. info
     * gets {width, height, format, has alpha} of those pixels. A sample of 0 gives only the size and format, and null.
     * Null when the image does not decode.
     */
    public static native int[] decode(byte[] data, int offset, int length, int sample, float scale, int[] info);

    /** The pixels (width * height) as a PNG (with an alpha channel when alpha) or a JPEG of quality 0 to 100, or null. */
    public static native byte[] encode(int[] pixels, int width, int height, int format, int quality, boolean alpha);
}
