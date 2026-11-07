// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.util;

/** A display's size and density. */
public class DisplayMetrics {
    public static final int DENSITY_LOW = 120;
    public static final int DENSITY_MEDIUM = 160;
    public static final int DENSITY_TV = 213;
    public static final int DENSITY_HIGH = 240;
    public static final int DENSITY_XHIGH = 320;
    public static final int DENSITY_400 = 400;
    public static final int DENSITY_XXHIGH = 480;
    public static final int DENSITY_XXXHIGH = 640;
    public static final int DENSITY_DEFAULT = DENSITY_MEDIUM;
    public static final float DENSITY_DEFAULT_SCALE = 1.0f / DENSITY_DEFAULT;
    public static int DENSITY_DEVICE_STABLE = DENSITY_XHIGH;

    public int widthPixels;
    public int heightPixels;
    public float density;
    public int densityDpi;
    public float scaledDensity;
    public float xdpi;
    public float ydpi;

    public DisplayMetrics() {}

    public void setTo(DisplayMetrics o) {
        if (this == o) return;
        widthPixels = o.widthPixels;
        heightPixels = o.heightPixels;
        density = o.density;
        densityDpi = o.densityDpi;
        scaledDensity = o.scaledDensity;
        xdpi = o.xdpi;
        ydpi = o.ydpi;
    }

    public void setToDefaults() {
        widthPixels = 0;
        heightPixels = 0;
        density = DENSITY_DEVICE_STABLE / (float) DENSITY_DEFAULT;
        densityDpi = DENSITY_DEVICE_STABLE;
        scaledDensity = density;
        xdpi = DENSITY_DEVICE_STABLE;
        ydpi = DENSITY_DEVICE_STABLE;
    }

    public boolean equals(DisplayMetrics o) {
        return o != null && widthPixels == o.widthPixels && heightPixels == o.heightPixels && density == o.density
            && densityDpi == o.densityDpi && scaledDensity == o.scaledDensity && xdpi == o.xdpi && ydpi == o.ydpi;
    }

    @Override
    public boolean equals(Object o) {
        return o instanceof DisplayMetrics d && equals(d);
    }

    @Override
    public int hashCode() {
        return widthPixels * heightPixels * densityDpi;
    }

    @Override
    public String toString() {
        return "DisplayMetrics{density=" + density + ", width=" + widthPixels + ", height=" + heightPixels
            + ", scaledDensity=" + scaledDensity + ", xdpi=" + xdpi + ", ydpi=" + ydpi + "}";
    }
}
