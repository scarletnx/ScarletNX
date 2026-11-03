// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.hardware.display;

import android.os.Handler;
import android.view.Display;

/** The displays: one, the default display of the window. */
public final class DisplayManager {
    public static final String DISPLAY_CATEGORY_PRESENTATION = "android.hardware.display.category.PRESENTATION";

    public interface DisplayListener {
        void onDisplayAdded(int displayId);
        void onDisplayRemoved(int displayId);
        void onDisplayChanged(int displayId);
    }

    public DisplayManager() {}

    public Display getDisplay(int displayId) { return displayId == Display.DEFAULT_DISPLAY ? Display.DEFAULT : null; }
    public Display[] getDisplays() { return new Display[] { Display.DEFAULT }; }
    public Display[] getDisplays(String category) { return category == null ? getDisplays() : new Display[0]; }
    public void registerDisplayListener(DisplayListener listener, Handler handler) {}
    public void unregisterDisplayListener(DisplayListener listener) {}
}
