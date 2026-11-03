// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** The formats of camera and video images. */
public class ImageFormat {
    public static final int UNKNOWN = 0, RGB_565 = 4, YV12 = 0x32315659, Y8 = 0x20203859, YCBCR_P010 = 0x36,
        YCBCR_P210 = 0x3c, NV16 = 0x10, NV21 = 0x11, YUY2 = 0x14, JPEG = 0x100, DEPTH_JPEG = 0x69656963,
        JPEG_R = 0x1005, HEIC = 0x48454946, HEIC_ULTRAHDR = 0x1006, YUV_420_888 = 0x23, YUV_422_888 = 0x27,
        YUV_444_888 = 0x28, FLEX_RGB_888 = 0x29, FLEX_RGBA_8888 = 0x2a, RAW_SENSOR = 0x20, RAW_PRIVATE = 0x24,
        RAW10 = 0x25, RAW12 = 0x26, DEPTH16 = 0x44363159, DEPTH_POINT_CLOUD = 0x101, PRIVATE = 0x22;

    public ImageFormat() {}

    /** The bits of one pixel, or -1 for a format without a fixed size per pixel. */
    public static int getBitsPerPixel(int format) {
        switch (format) {
            case Y8: return 8;
            case RAW10: return 10;
            case YV12: case NV21: case YUV_420_888: case RAW12: return 12;
            case RGB_565: case NV16: case YUY2: case YUV_422_888: case RAW_SENSOR: case DEPTH16: return 16;
            case YUV_444_888: case FLEX_RGB_888: case YCBCR_P010: return 24;
            case FLEX_RGBA_8888: case YCBCR_P210: return 32;
            default: return -1;
        }
    }
}
