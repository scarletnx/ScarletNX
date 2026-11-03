// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media;

import android.graphics.Bitmap;
import android.os.CancellationSignal;
import android.util.Size;
import java.io.File;
import java.io.IOException;

/** Thumbnails of media files. No video decoder is here, so no video gives a frame. */
public class ThumbnailUtils {
    public static final int OPTIONS_RECYCLE_INPUT = 2;

    public ThumbnailUtils() {}

    @Deprecated public static Bitmap createVideoThumbnail(String filePath, int kind) { return null; }

    public static Bitmap createVideoThumbnail(File file, Size size, CancellationSignal signal) throws IOException {
        if (signal != null) signal.throwIfCanceled();
        throw new IOException("snx: no video decoder for " + file);
    }
}
