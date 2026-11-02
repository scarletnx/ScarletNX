// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android;

import android.content.res.Configuration;
import android.util.DisplayMetrics;
import java.util.Locale;

/**
 * The device the app runs on: one display, the window's, and the configuration that follows from it. The runtime
 * sets its size and density (snx.android.width, snx.android.height in pixels, snx.android.dpi); without them a
 * phone-shaped window.
 */
public final class Device {
    private static final DisplayMetrics metrics = new DisplayMetrics();
    private static final Configuration configuration = new Configuration();

    static {
        int dpi = Integer.getInteger("snx.android.dpi", 240);
        int width = Integer.getInteger("snx.android.width", 600);
        int height = Integer.getInteger("snx.android.height", 1066);
        boolean night = Boolean.getBoolean("snx.android.night");
        set(width, height, dpi, night);
    }

    private Device() {}

    /** The display's size in pixels and its density: the metrics and the configuration again. */
    public static synchronized void set(int width, int height, int dpi, boolean night) {
        metrics.widthPixels = width;
        metrics.heightPixels = height;
        metrics.densityDpi = dpi;
        metrics.density = dpi / 160f;
        metrics.scaledDensity = metrics.density;
        metrics.xdpi = metrics.ydpi = dpi;
        DisplayMetrics.DENSITY_DEVICE_STABLE = dpi;
        Configuration c = configuration;
        c.setToDefaults();
        c.fontScale = 1;
        c.setLocale(Locale.getDefault());
        c.densityDpi = dpi;
        c.screenWidthDp = (int) (width / metrics.density);
        c.screenHeightDp = (int) (height / metrics.density);
        c.smallestScreenWidthDp = Math.min(c.screenWidthDp, c.screenHeightDp);
        c.orientation = height >= width ? Configuration.ORIENTATION_PORTRAIT : Configuration.ORIENTATION_LANDSCAPE;
        int size = c.smallestScreenWidthDp >= 720 ? Configuration.SCREENLAYOUT_SIZE_XLARGE
            : c.smallestScreenWidthDp >= 600 ? Configuration.SCREENLAYOUT_SIZE_LARGE
            : Configuration.SCREENLAYOUT_SIZE_NORMAL;
        boolean tall = Math.max(c.screenWidthDp, c.screenHeightDp) * 3 > Math.min(c.screenWidthDp, c.screenHeightDp) * 5;
        c.screenLayout = size | (tall ? Configuration.SCREENLAYOUT_LONG_YES : Configuration.SCREENLAYOUT_LONG_NO)
            | Configuration.SCREENLAYOUT_LAYOUTDIR_LTR | Configuration.SCREENLAYOUT_ROUND_NO;
        c.uiMode = Configuration.UI_MODE_TYPE_NORMAL
            | (night ? Configuration.UI_MODE_NIGHT_YES : Configuration.UI_MODE_NIGHT_NO);
        c.touchscreen = Configuration.TOUCHSCREEN_FINGER;
        c.keyboard = Configuration.KEYBOARD_QWERTY;
        c.keyboardHidden = Configuration.KEYBOARDHIDDEN_NO;
        c.hardKeyboardHidden = Configuration.HARDKEYBOARDHIDDEN_NO;
        c.navigation = Configuration.NAVIGATION_NONAV;
        c.navigationHidden = Configuration.NAVIGATIONHIDDEN_YES;
        c.fontWeightAdjustment = 0;
        c.seq = 1;
    }

    /** A copy of the display's metrics. */
    public static synchronized DisplayMetrics metrics() {
        DisplayMetrics m = new DisplayMetrics();
        m.setTo(metrics);
        return m;
    }

    /** A copy of the device's configuration. */
    public static synchronized Configuration configuration() {
        return new Configuration(configuration);
    }
}
