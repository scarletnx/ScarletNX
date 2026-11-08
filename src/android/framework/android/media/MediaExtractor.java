// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media;

import android.content.Context;
import android.net.Uri;
import java.io.File;
import java.io.FileDescriptor;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.Map;

/** Demuxes media files. No demuxer is here: a readable source opens as a container with no tracks. */
public final class MediaExtractor {
    public static final int SEEK_TO_PREVIOUS_SYNC = 0, SEEK_TO_NEXT_SYNC = 1, SEEK_TO_CLOSEST_SYNC = 2;
    public static final int SAMPLE_FLAG_SYNC = 1, SAMPLE_FLAG_ENCRYPTED = 2, SAMPLE_FLAG_PARTIAL_FRAME = 4;

    public MediaExtractor() {}

    public void setDataSource(String path) throws IOException {
        if (path.contains("://") && !path.startsWith("file://")) return;
        File f = new File(path.startsWith("file://") ? path.substring(7) : path);
        if (!f.canRead()) throw new IOException("Failed to instantiate extractor.");
    }
    public void setDataSource(String path, Map<String, String> headers) throws IOException { setDataSource(path); }
    public void setDataSource(Context context, Uri uri, Map<String, String> headers) throws IOException {}
    public void setDataSource(FileDescriptor fd) throws IOException {}
    public void setDataSource(FileDescriptor fd, long offset, long length) throws IOException {}
    public void setDataSource(android.content.res.AssetFileDescriptor afd) throws IOException {}

    public int getTrackCount() { return 0; }
    public MediaFormat getTrackFormat(int index) { throw new IllegalArgumentException("track " + index + " does not exist"); }
    public void selectTrack(int index) { throw new IllegalArgumentException("track " + index + " does not exist"); }
    public void unselectTrack(int index) { throw new IllegalArgumentException("track " + index + " does not exist"); }
    public void seekTo(long timeUs, int mode) {}
    public boolean advance() { return false; }
    public int readSampleData(ByteBuffer byteBuf, int offset) { return -1; }
    public int getSampleTrackIndex() { return -1; }
    public long getSampleTime() { return -1; }
    public long getSampleSize() { return -1; }
    public int getSampleFlags() { return -1; }
    public long getCachedDuration() { return -1; }
    public boolean hasCacheReachedEndOfStream() { return true; }
    public void release() {}
}
