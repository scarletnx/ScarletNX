// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.content.Context;
import android.hardware.SensorManager;

/**
 * Tells the device's orientation in degrees from its accelerometer. The device here has no accelerometer
 * (SensorManager lists none), so canDetectOrientation is false and enable starts nothing, as Android's does then.
 */
public abstract class OrientationEventListener {
    public static final int ORIENTATION_UNKNOWN = -1;

    public OrientationEventListener(Context context) { this(context, SensorManager.SENSOR_DELAY_NORMAL); }
    public OrientationEventListener(Context context, int rate) {}

    public void enable() {
        if (!canDetectOrientation()) android.util.Log.w("OrientationEventListener", "Cannot detect sensors. Not enabled");
    }
    public void disable() {}
    public boolean canDetectOrientation() { return false; }
    /** 0 to 359 degrees, or ORIENTATION_UNKNOWN when the device lies flat */
    public abstract void onOrientationChanged(int orientation);
}
