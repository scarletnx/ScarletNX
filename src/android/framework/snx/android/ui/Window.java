// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.ui;

/**
 * The host window: an SDL window with the runtime's Vulkan renderer (src/android/hwui/). Without the natives (a
 * build without a window, or SNX_ANDROID_HEADLESS) frames go nowhere and the process runs headless.
 */
public final class Window {
    private static boolean open;
    private static boolean headless = System.getenv("SNX_ANDROID_HEADLESS") != null;

    private Window() {}

    static synchronized void present(Frame frame) {
        if (headless) return;
        try {
            if (!open) {
                open = nativeOpen(frame.width, frame.height, snx.android.Main.processName());
                if (!open) { headless = true; return; }
            }
            nativePresent(frame.bytes(), frame.size(), frame.bitmaps().toArray(new android.graphics.Bitmap[0]));
        } catch (UnsatisfiedLinkError e) {
            headless = true;
            android.util.Log.w("snx", "no renderer in this runtime: running headless");
        }
    }

    private static native boolean nativeOpen(int width, int height, String title);
    private static native void nativePresent(byte[] list, int size, android.graphics.Bitmap[] bitmaps);
}
