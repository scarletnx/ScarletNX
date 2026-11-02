// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

import snx.android.hwui.DisplayList;

/** The canvas a RenderNode records into. */
public class RecordingCanvas extends Canvas {
    final RenderNode node;

    RecordingCanvas(RenderNode node, DisplayList list, int width, int height) {
        super(list, width, height);
        this.node = node;
    }

    void restart(DisplayList list, int width, int height) {
        this.list = list;
        w = list.writer();
        begin(width, height);
    }
}
