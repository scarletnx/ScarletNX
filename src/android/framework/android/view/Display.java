// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.graphics.Point;
import android.graphics.Rect;
import android.util.DisplayMetrics;

/**
 * A display: the default one is the host window, at its size and density (snx.android.Device); a virtual one
 * (DisplayManager.createVirtualDisplay) has its own, and what shows on it goes nowhere.
 */
public final class Display {
    public static final int DEFAULT_DISPLAY = 0, INVALID_DISPLAY = -1, STATE_UNKNOWN = 0, STATE_OFF = 1, STATE_ON = 2;
    public static final int FLAG_SUPPORTS_PROTECTED_BUFFERS = 1, FLAG_SECURE = 2, FLAG_PRIVATE = 4, FLAG_PRESENTATION = 8,
        FLAG_ROUND = 16;
    public static final Display DEFAULT = new Display(DEFAULT_DISPLAY, "ScarletNX", 0, 0, 0, 0);

    private final int id, flags, width, height, densityDpi;
    private final String name;
    private volatile boolean valid = true;

    private Display(int id, String name, int flags, int width, int height, int densityDpi) {
        this.id = id;
        this.name = name;
        this.flags = flags;
        this.width = width;
        this.height = height;
        this.densityDpi = densityDpi;
    }
    /** a virtual display of the runtime's (DisplayManager.createVirtualDisplay's) */
    public static Display virtual(int id, String name, int flags, int width, int height, int densityDpi) {
        return new Display(id, name, flags, width, height, densityDpi);
    }
    /** a virtual display released: no longer valid */
    public void invalidate() { valid = false; }
    private boolean isDefault() { return id == DEFAULT_DISPLAY; }

    public int getDisplayId() { return id; }
    public String getName() { return name; }
    public boolean isValid() { return valid; }
    public int getState() { return valid ? STATE_ON : STATE_OFF; }
    public int getFlags() { return flags; }
    public int getRotation() { return Surface.ROTATION_0; }
    @Deprecated public int getOrientation() { return 0; }
    public float getRefreshRate() { return 60; }
    public float[] getSupportedRefreshRates() { return new float[] {60}; }
    public Mode getMode() { return new Mode(); }
    public Mode[] getSupportedModes() { return new Mode[] {new Mode()}; }
    public long getAppVsyncOffsetNanos() { return 0; }
    public long getPresentationDeadlineNanos() { return 16_666_667L; }
    public boolean isHdr() { return false; }
    public boolean isWideColorGamut() { return false; }
    public HdrCapabilities getHdrCapabilities() { return null; }
    public DisplayCutout getCutout() { return null; }
    /** the default display's metrics, or a virtual one's: its size and density */
    private DisplayMetrics metrics() {
        DisplayMetrics m = new DisplayMetrics();
        m.setTo(snx.android.Device.metrics());
        if (isDefault()) return m;
        m.widthPixels = width;
        m.heightPixels = height;
        m.densityDpi = densityDpi;
        m.density = densityDpi / 160f;
        m.scaledDensity = m.density;
        m.xdpi = m.ydpi = densityDpi;
        return m;
    }
    public void getMetrics(DisplayMetrics outMetrics) { outMetrics.setTo(metrics()); }
    public void getRealMetrics(DisplayMetrics outMetrics) { getMetrics(outMetrics); }
    public void getSize(Point outSize) { DisplayMetrics m = metrics(); outSize.set(m.widthPixels, m.heightPixels); }
    public void getRealSize(Point outSize) { getSize(outSize); }
    public void getRectSize(Rect outSize) { DisplayMetrics m = metrics(); outSize.set(0, 0, m.widthPixels, m.heightPixels); }
    public void getCurrentSizeRange(Point outSmallestSize, Point outLargestSize) { getSize(outSmallestSize); getSize(outLargestSize); }
    @Deprecated public int getWidth() { return metrics().widthPixels; }
    @Deprecated public int getHeight() { return metrics().heightPixels; }
    @Override public String toString() { return "Display id " + id + ": " + name; }

    public static final class Mode {
        public int getModeId() { return 1; }
        public int getPhysicalWidth() { return snx.android.Device.metrics().widthPixels; }
        public int getPhysicalHeight() { return snx.android.Device.metrics().heightPixels; }
        public float getRefreshRate() { return 60; }
        public float[] getAlternativeRefreshRates() { return new float[0]; }
    }

    public static final class HdrCapabilities {
        public int[] getSupportedHdrTypes() { return new int[0]; }
    }
}
