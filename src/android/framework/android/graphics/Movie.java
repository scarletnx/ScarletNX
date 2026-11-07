// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

import java.io.InputStream;

/** An animated image. Only the first frame is decoded here, so a movie has a duration of zero. */
@Deprecated
public class Movie {
    private final Bitmap frame;

    private Movie(Bitmap frame) { this.frame = frame; }

    public static Movie decodeStream(InputStream is) {
        Bitmap b = BitmapFactory.decodeStream(is);
        return b != null ? new Movie(b) : null;
    }
    public static Movie decodeByteArray(byte[] data, int offset, int length) {
        Bitmap b = BitmapFactory.decodeByteArray(data, offset, length);
        return b != null ? new Movie(b) : null;
    }
    public static Movie decodeFile(String pathName) {
        Bitmap b = BitmapFactory.decodeFile(pathName);
        return b != null ? new Movie(b) : null;
    }
    public int width() { return frame.getWidth(); }
    public int height() { return frame.getHeight(); }
    public boolean isOpaque() { return !frame.hasAlpha(); }
    public int duration() { return 0; }
    public boolean setTime(int relativeMilliseconds) { return true; }
    public void draw(Canvas canvas, float x, float y, Paint paint) { canvas.drawBitmap(frame, x, y, paint); }
    public void draw(Canvas canvas, float x, float y) { draw(canvas, x, y, null); }
}
