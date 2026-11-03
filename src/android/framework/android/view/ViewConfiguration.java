// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.content.Context;

/** Touch slop, timeouts and velocities, scaled by the display's density. */
public class ViewConfiguration {
    private final float density;

    private ViewConfiguration(float density) { this.density = density; }
    public static ViewConfiguration get(Context context) { return new ViewConfiguration(snx.android.Device.metrics().density); }
    public static int getTapTimeout() { return 100; }
    public static int getDoubleTapTimeout() { return 300; }
    public static int getLongPressTimeout() { return 400; }
    public static int getKeyRepeatTimeout() { return 400; }
    public static int getKeyRepeatDelay() { return 50; }
    public static int getPressedStateDuration() { return 64; }
    public static int getJumpTapTimeout() { return 500; }
    public static int getScrollBarFadeDuration() { return 250; }
    public static int getScrollDefaultDelay() { return 300; }
    public static long getZoomControlsTimeout() { return 3000; }
    public static long getGlobalActionKeyTimeout() { return 500; }
    public static float getScrollFriction() { return 0.015f; }
    public static int getMultiPressTimeout() { return 300; }
    public static int getHoverTooltipShowTimeout() { return 500; }
    public static int getHoverTooltipHideTimeout() { return 15000; }
    public static int getEdgeSlop() { return 12; }
    @Deprecated public static int getScrollBarSize() { return 4; }
    @Deprecated public static int getFadingEdgeLength() { return 12; }
    @Deprecated public static int getWindowTouchSlop() { return 16; }
    @Deprecated public static int getMinimumFlingVelocity() { return 50; }
    @Deprecated public static int getMaximumFlingVelocity() { return 8000; }
    @Deprecated public static int getMaximumDrawingCacheSize() { return 480 * 800 * 4; }
    public static long getDefaultActionModeHideDuration() { return 2000; }
    @Deprecated public static int getTouchSlop() { return 8; }
    public int getScaledTouchSlop() { return (int) (8 * density + 0.5f); }
    public int getScaledHoverSlop() { return (int) (4 * density + 0.5f); }
    public int getScaledPagingTouchSlop() { return getScaledTouchSlop() * 2; }
    public int getScaledDoubleTapSlop() { return (int) (100 * density + 0.5f); }
    public int getScaledEdgeSlop() { return (int) (12 * density + 0.5f); }
    public int getScaledWindowTouchSlop() { return (int) (16 * density + 0.5f); }
    public int getScaledMinimumFlingVelocity() { return (int) (50 * density + 0.5f); }
    public int getScaledMaximumFlingVelocity() { return (int) (8000 * density + 0.5f); }
    public int getScaledMinimumFlingVelocity(int inputDeviceId, int axis, int source) { return getScaledMinimumFlingVelocity(); }
    public int getScaledMaximumFlingVelocity(int inputDeviceId, int axis, int source) { return getScaledMaximumFlingVelocity(); }
    public int getScaledScrollBarSize() { return (int) (4 * density + 0.5f); }
    public int getScaledFadingEdgeLength() { return (int) (12 * density + 0.5f); }
    public int getScaledOverscrollDistance() { return 0; }
    public int getScaledOverflingDistance() { return (int) (6 * density + 0.5f); }
    public float getScaledHorizontalScrollFactor() { return 64 * density; }
    public float getScaledVerticalScrollFactor() { return 64 * density; }
    public int getScaledMinimumScalingSpan() { return (int) (170 * density + 0.5f); }
    public int getScaledHandwritingSlop() { return (int) (2 * density + 0.5f); }
    public int getScaledHandwritingGestureLineMargin() { return (int) (16 * density + 0.5f); }
    public boolean hasPermanentMenuKey() { return false; }
    public boolean shouldShowMenuShortcutsWhenKeyboardPresent() { return false; }
    public static boolean isScreenshotChordEnabled() { return false; }
}
