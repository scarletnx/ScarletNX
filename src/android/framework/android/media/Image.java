// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media;

import java.nio.ByteBuffer;

/** A frame of media data, as planes of pixels. */
public abstract class Image implements AutoCloseable {
    protected Image() {}

    public abstract int getFormat();
    public abstract int getWidth();
    public abstract int getHeight();
    public abstract long getTimestamp();
    public void setTimestamp(long timestamp) {}
    public android.graphics.Rect getCropRect() { return new android.graphics.Rect(0, 0, getWidth(), getHeight()); }
    public void setCropRect(android.graphics.Rect cropRect) {}
    public abstract Plane[] getPlanes();
    @Override public abstract void close();

    public abstract static class Plane {
        protected Plane() {}
        public abstract int getRowStride();
        public abstract int getPixelStride();
        public abstract ByteBuffer getBuffer();
    }
}
