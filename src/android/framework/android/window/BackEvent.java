// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.window;

/** A back gesture's progress. */
public final class BackEvent {
    public static final int EDGE_LEFT = 0, EDGE_RIGHT = 1, EDGE_NONE = 2;
    private final float x, y, progress;
    private final int edge;
    public BackEvent(float touchX, float touchY, float progress, int swipeEdge) { x = touchX; y = touchY; this.progress = progress; edge = swipeEdge; }
    public float getTouchX() { return x; }
    public float getTouchY() { return y; }
    public float getProgress() { return progress; }
    public int getSwipeEdge() { return edge; }
}
