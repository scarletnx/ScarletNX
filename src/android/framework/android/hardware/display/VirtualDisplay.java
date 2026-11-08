// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.hardware.display;

import android.view.Display;
import android.view.Surface;

/** A virtual display (DisplayManager.createVirtualDisplay): its display, and the surface its content would go to. */
public final class VirtualDisplay {
    public abstract static class Callback {
        public void onPaused() {}
        public void onResumed() {}
        public void onStopped() {}
    }

    private final DisplayManager manager;
    private final Display display;
    private Surface surface;

    VirtualDisplay(DisplayManager manager, Display display, Surface surface) {
        this.manager = manager;
        this.display = display;
        this.surface = surface;
    }
    public Display getDisplay() { return display; }
    public Surface getSurface() { return surface; }
    public void setSurface(Surface surface) { this.surface = surface; }
    public void resize(int width, int height, int densityDpi) {}
    public void release() { manager.release(display); }
    @Override public String toString() { return "VirtualDisplay{display=" + display + "}"; }
}
