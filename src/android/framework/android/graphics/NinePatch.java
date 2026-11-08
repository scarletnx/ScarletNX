// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/**
 * A bitmap whose stretchable parts (the divs of its chunk, Res_png_9patch in a device's order) stretch to fill a
 * rectangle, and whose other parts keep their size, as libhwui's nine-patch draws it.
 */
public class NinePatch {
    private final Bitmap bitmap;
    private final int[] xDivs, yDivs;

    public NinePatch(Bitmap bitmap, byte[] chunk) { this(bitmap, chunk, null); }
    public NinePatch(Bitmap bitmap, byte[] chunk, String srcName) {
        this.bitmap = bitmap;
        boolean valid = isNinePatchChunk(chunk);
        int nx = valid ? chunk[1] & 0xff : 0, ny = valid ? chunk[2] & 0xff : 0;
        if (valid && chunk.length < 32 + 4 * (nx + ny)) nx = ny = 0;
        xDivs = divs(chunk, 32, nx);
        yDivs = divs(chunk, 32 + 4 * nx, ny);
    }
    private static int[] divs(byte[] chunk, int at, int count) {
        int[] d = new int[count & ~1];
        for (int i = 0; i < d.length; ++i) d[i] = BitmapFactory.chunk32(chunk, at + 4 * i);
        return d;
    }
    public Bitmap getBitmap() { return bitmap; }
    public int getWidth() { return bitmap.getWidth(); }
    public int getHeight() { return bitmap.getHeight(); }
    public void draw(Canvas canvas, RectF location) { canvas.drawPatch(this, location, null); }
    public void draw(Canvas canvas, Rect location) { canvas.drawPatch(this, location, null); }
    public void draw(Canvas canvas, Rect location, Paint paint) { canvas.drawPatch(this, location, paint); }
    /** As Android's native check: the chunk is long enough and was not marked as not deserialized (-1) */
    public static boolean isNinePatchChunk(byte[] chunk) { return chunk != null && chunk.length >= 32 && chunk[0] != -1; }

    /** The edges of the parts along one side: source edges into src, destination edges into dst, both size + 1 */
    private static int edges(int[] divs, int size, float left, float right, float[] src, float[] dst) {
        int n = 0;
        src[n++] = 0;
        for (int d : divs) {
            float e = Math.max(src[n - 1], Math.min(size, d));
            src[n++] = e;
        }
        src[n++] = size;
        float stretch = 0;
        for (int i = 1; i < n; ++i) if ((i & 1) == 0) stretch += src[i] - src[i - 1];
        float fixed = size - stretch, room = right - left;
        /* the fixed parts shrink together when the rectangle is smaller than they are, and the stretchable ones share
           what is left by their size in the bitmap */
        float fixedScale = fixed > room && fixed > 0 ? room / fixed : 1;
        float extra = Math.max(0, room - fixed);
        dst[0] = left;
        for (int i = 1; i < n; ++i) {
            float part = src[i] - src[i - 1];
            boolean stretchable = (i & 1) == 0;
            float out = stretchable ? (stretch > 0 ? extra * part / stretch : 0) : part * fixedScale;
            dst[i] = dst[i - 1] + out;
        }
        dst[n - 1] = right;
        return n;
    }

    /** Draws each part of the bitmap into its part of location */
    void drawStretched(Canvas canvas, RectF location, Paint paint) {
        if (xDivs.length == 0 && yDivs.length == 0) {
            canvas.drawBitmap(bitmap, null, location, paint);
            return;
        }
        float[] sx = new float[xDivs.length + 2], dx = new float[xDivs.length + 2];
        float[] sy = new float[yDivs.length + 2], dy = new float[yDivs.length + 2];
        int nx = edges(xDivs, bitmap.getWidth(), location.left, location.right, sx, dx);
        int ny = edges(yDivs, bitmap.getHeight(), location.top, location.bottom, sy, dy);
        Rect src = new Rect();
        RectF dst = new RectF();
        for (int j = 1; j < ny; ++j) {
            if (sy[j] <= sy[j - 1] || dy[j] <= dy[j - 1]) continue;
            for (int i = 1; i < nx; ++i) {
                if (sx[i] <= sx[i - 1] || dx[i] <= dx[i - 1]) continue;
                src.set((int) sx[i - 1], (int) sy[j - 1], (int) sx[i], (int) sy[j]);
                dst.set(dx[i - 1], dy[j - 1], dx[i], dy[j]);
                canvas.drawBitmap(bitmap, src, dst, paint);
            }
        }
    }
}
