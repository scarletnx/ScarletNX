// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.hardware;

import android.os.Parcel;
import android.os.Parcelable;

/** A buffer that the GPU and other hardware share. No bitmap or surface here gives one. */
public final class HardwareBuffer implements Parcelable, AutoCloseable {
    public static final int RGBA_8888 = 1, RGBX_8888 = 2, RGB_888 = 3, RGB_565 = 4, RGBA_FP16 = 0x16,
        RGBA_1010102 = 0x2b, BLOB = 0x21, YCBCR_420_888 = 0x23, D_16 = 0x30, D_24 = 0x31, DS_24UI8 = 0x32,
        D_FP32 = 0x33, DS_FP32UI8 = 0x34, S_UI8 = 0x35, R_8 = 0x38;
    public static final long USAGE_CPU_READ_RARELY = 2, USAGE_CPU_READ_OFTEN = 3, USAGE_CPU_WRITE_RARELY = 2 << 4,
        USAGE_CPU_WRITE_OFTEN = 3 << 4, USAGE_GPU_SAMPLED_IMAGE = 1 << 8, USAGE_GPU_COLOR_OUTPUT = 1 << 9,
        USAGE_PROTECTED_CONTENT = 1 << 14, USAGE_VIDEO_ENCODE = 1 << 16, USAGE_GPU_DATA_BUFFER = 1 << 24;

    private final int width, height, format, layers;
    private final long usage;
    private boolean closed;

    private HardwareBuffer(int width, int height, int format, int layers, long usage) {
        this.width = width; this.height = height; this.format = format; this.layers = layers; this.usage = usage;
    }

    private void check() { if (closed) throw new IllegalStateException("This HardwareBuffer has been closed and its dimensions cannot be obtained."); }

    public int getWidth() { check(); return width; }
    public int getHeight() { check(); return height; }
    public int getFormat() { check(); return format; }
    public int getLayers() { check(); return layers; }
    public long getUsage() { check(); return usage; }
    public boolean isClosed() { return closed; }
    @Override public void close() { closed = true; }

    @Override public int describeContents() { return Parcelable.CONTENTS_FILE_DESCRIPTOR; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<HardwareBuffer> CREATOR = new Parcelable.Creator<>() {
        @Override public HardwareBuffer createFromParcel(Parcel in) { return (HardwareBuffer) in.readValue(null); }
        @Override public HardwareBuffer[] newArray(int size) { return new HardwareBuffer[size]; }
    };
}
