// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2007 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.media;

import java.io.File;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.HashMap;

/**
 * The Exif tags of a JPEG file: the TIFF entries of its APP1 segment, from IFD0 and the Exif IFD. Other files have
 * no tags here.
 */
public class ExifInterface {
    public static final String TAG_ORIENTATION = "Orientation", TAG_IMAGE_WIDTH = "ImageWidth", TAG_IMAGE_LENGTH = "ImageLength",
        TAG_MAKE = "Make", TAG_MODEL = "Model", TAG_DATETIME = "DateTime", TAG_DATETIME_ORIGINAL = "DateTimeOriginal",
        TAG_PIXEL_X_DIMENSION = "PixelXDimension", TAG_PIXEL_Y_DIMENSION = "PixelYDimension", TAG_SOFTWARE = "Software",
        TAG_FLASH = "Flash", TAG_WHITE_BALANCE = "WhiteBalance";
    public static final int ORIENTATION_UNDEFINED = 0, ORIENTATION_NORMAL = 1, ORIENTATION_FLIP_HORIZONTAL = 2,
        ORIENTATION_ROTATE_180 = 3, ORIENTATION_FLIP_VERTICAL = 4, ORIENTATION_TRANSPOSE = 5, ORIENTATION_ROTATE_90 = 6,
        ORIENTATION_TRANSVERSE = 7, ORIENTATION_ROTATE_270 = 8;

    private static final HashMap<Integer, String> NAMES = new HashMap<>();
    static {
        NAMES.put(0x0100, TAG_IMAGE_WIDTH); NAMES.put(0x0101, TAG_IMAGE_LENGTH); NAMES.put(0x010f, TAG_MAKE);
        NAMES.put(0x0110, TAG_MODEL); NAMES.put(0x0112, TAG_ORIENTATION); NAMES.put(0x0131, TAG_SOFTWARE);
        NAMES.put(0x0132, TAG_DATETIME); NAMES.put(0x9003, TAG_DATETIME_ORIGINAL); NAMES.put(0x9209, TAG_FLASH);
        NAMES.put(0xa002, TAG_PIXEL_X_DIMENSION); NAMES.put(0xa003, TAG_PIXEL_Y_DIMENSION); NAMES.put(0xa403, TAG_WHITE_BALANCE);
    }

    private final HashMap<String, String> attributes = new HashMap<>();

    public ExifInterface(String filename) throws IOException {
        if (filename == null) throw new NullPointerException("filename cannot be null");
        try (InputStream in = new FileInputStream(filename)) { load(in); }
    }
    public ExifInterface(File file) throws IOException {
        if (file == null) throw new NullPointerException("file cannot be null");
        try (InputStream in = new FileInputStream(file)) { load(in); }
    }
    public ExifInterface(FileDescriptor fileDescriptor) throws IOException { load(new FileInputStream(fileDescriptor)); }
    public ExifInterface(InputStream inputStream) throws IOException {
        if (inputStream == null) throw new NullPointerException("inputStream cannot be null");
        load(inputStream);
    }

    public String getAttribute(String tag) { return attributes.get(tag); }
    public boolean hasAttribute(String tag) { return attributes.containsKey(tag); }

    public int getAttributeInt(String tag, int defaultValue) {
        String v = attributes.get(tag);
        if (v == null) return defaultValue;
        try { return Integer.parseInt(v); } catch (NumberFormatException e) { return defaultValue; }
    }

    public double getAttributeDouble(String tag, double defaultValue) {
        String v = attributes.get(tag);
        if (v == null) return defaultValue;
        try { return Double.parseDouble(v); } catch (NumberFormatException e) { return defaultValue; }
    }

    private void load(InputStream in) throws IOException {
        byte[] head = in.readNBytes(2);
        if (head.length < 2 || (head[0] & 0xff) != 0xff || (head[1] & 0xff) != 0xd8) return;
        while (true) {
            int marker = in.read();
            if (marker != 0xff) return;
            int type = in.read();
            if (type == 0xd9 || type == 0xda || type < 0) return;
            byte[] lenBytes = in.readNBytes(2);
            if (lenBytes.length < 2) return;
            int len = ((lenBytes[0] & 0xff) << 8 | (lenBytes[1] & 0xff)) - 2;
            if (len < 0) return;
            byte[] segment = in.readNBytes(len);
            if (segment.length < len) return;
            if (type == 0xe1 && len > 14 && segment[0] == 'E' && segment[1] == 'x' && segment[2] == 'i' && segment[3] == 'f') {
                parseTiff(ByteBuffer.wrap(segment, 6, len - 6).slice());
                return;
            }
        }
    }

    private void parseTiff(ByteBuffer b) {
        if (b.remaining() < 8) return;
        b.order(b.get(0) == 'I' ? ByteOrder.LITTLE_ENDIAN : ByteOrder.BIG_ENDIAN);
        int exifIfd = parseIfd(b, b.getInt(4));
        if (exifIfd > 0) parseIfd(b, exifIfd);
    }

    /** One IFD's known tags; the offset of the Exif IFD, or 0. */
    private int parseIfd(ByteBuffer b, int offset) {
        if (offset < 0 || offset + 2 > b.limit()) return 0;
        int count = b.getShort(offset) & 0xffff, exif = 0;
        for (int i = 0; i < count; ++i) {
            int e = offset + 2 + 12 * i;
            if (e + 12 > b.limit()) break;
            int tag = b.getShort(e) & 0xffff, format = b.getShort(e + 2) & 0xffff, n = b.getInt(e + 4);
            if (tag == 0x8769) { exif = b.getInt(e + 8); continue; }
            String name = NAMES.get(tag);
            if (name == null || n <= 0) continue;
            switch (format) {
                case 3 -> attributes.put(name, Integer.toString(b.getShort(e + 8) & 0xffff));
                case 4 -> attributes.put(name, Long.toString(b.getInt(e + 8) & 0xffffffffL));
                case 2 -> {
                    int at = n <= 4 ? e + 8 : b.getInt(e + 8);
                    if (at < 0 || at + n > b.limit()) break;
                    StringBuilder s = new StringBuilder();
                    for (int k = 0; k < n && b.get(at + k) != 0; ++k) s.append((char) (b.get(at + k) & 0xff));
                    attributes.put(name, s.toString());
                }
                default -> {}
            }
        }
        return exif;
    }
}
