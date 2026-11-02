// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.graphics.Point;
import android.graphics.Rect;
import android.util.DisplayMetrics;

/** The display: the window's size and density (snx.android.Device). */
public final class Display {
    public static final int DEFAULT_DISPLAY = 0, INVALID_DISPLAY = -1, STATE_ON = 2, FLAG_SECURE = 2;
    public static final Display DEFAULT = new Display();

    private Display() {}

    public int getDisplayId() { return DEFAULT_DISPLAY; }
    public String getName() { return "ScarletNX"; }
    public boolean isValid() { return true; }
    public int getState() { return STATE_ON; }
    public int getFlags() { return 0; }
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
    public void getMetrics(DisplayMetrics outMetrics) { outMetrics.setTo(snx.android.Device.metrics()); }
    public void getRealMetrics(DisplayMetrics outMetrics) { getMetrics(outMetrics); }
    public void getSize(Point outSize) { DisplayMetrics m = snx.android.Device.metrics(); outSize.set(m.widthPixels, m.heightPixels); }
    public void getRealSize(Point outSize) { getSize(outSize); }
    public void getRectSize(Rect outSize) { DisplayMetrics m = snx.android.Device.metrics(); outSize.set(0, 0, m.widthPixels, m.heightPixels); }
    public void getCurrentSizeRange(Point outSmallestSize, Point outLargestSize) { getSize(outSmallestSize); getSize(outLargestSize); }
    @Deprecated public int getWidth() { return snx.android.Device.metrics().widthPixels; }
    @Deprecated public int getHeight() { return snx.android.Device.metrics().heightPixels; }

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
