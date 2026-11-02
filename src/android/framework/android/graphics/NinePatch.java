// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** A bitmap stretched in its middle parts (drawn stretched whole here). */
public class NinePatch {
    private final Bitmap bitmap;

    public NinePatch(Bitmap bitmap, byte[] chunk) { this.bitmap = bitmap; }
    public NinePatch(Bitmap bitmap, byte[] chunk, String srcName) { this.bitmap = bitmap; }
    public Bitmap getBitmap() { return bitmap; }
    public int getWidth() { return bitmap.getWidth(); }
    public int getHeight() { return bitmap.getHeight(); }
    public void draw(Canvas canvas, RectF location) { canvas.drawPatch(this, location, null); }
    public void draw(Canvas canvas, Rect location) { canvas.drawPatch(this, location, null); }
    public static boolean isNinePatchChunk(byte[] chunk) { return chunk != null && chunk.length >= 32 && chunk[0] != 0; }
}
