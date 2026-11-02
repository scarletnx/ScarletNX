// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

import snx.android.hwui.DisplayList;

/** Recorded drawing, drawn again where a canvas draws it. */
public class Picture {
    private DisplayList list = new DisplayList();
    private int width, height;
    private Canvas recording;

    public Picture() {}
    public Picture(Picture src) { list = src.list; width = src.width; height = src.height; }

    public Canvas beginRecording(int width, int height) {
        this.width = width;
        this.height = height;
        list = new DisplayList();
        recording = new Canvas(list, width, height);
        return recording;
    }
    public void endRecording() {
        if (recording != null) list.finish();
        recording = null;
    }
    public int getWidth() { return width; }
    public int getHeight() { return height; }
    public boolean requiresHardwareAcceleration() { return false; }
    public void draw(Canvas canvas) { canvas.drawPicture(this); }
    DisplayList list() { return list; }
}
