// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media;

/** Media metadata: none is read. */
public class MediaMetadataRetriever implements AutoCloseable {
    public static final int METADATA_KEY_DURATION = 9, METADATA_KEY_MIMETYPE = 12, METADATA_KEY_VIDEO_WIDTH = 18, METADATA_KEY_VIDEO_HEIGHT = 19;
    public void setDataSource(String path) {}
    public void setDataSource(android.content.Context context, android.net.Uri uri) {}
    public void setDataSource(java.io.FileDescriptor fd) {}
    public String extractMetadata(int keyCode) { return null; }
    public android.graphics.Bitmap getFrameAtTime(long timeUs) { return null; }
    @Override public void close() {}
    public void release() {}
}
