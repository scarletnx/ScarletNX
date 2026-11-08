// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.hardware;

/** A sensor of the device (Off in docs/platforms/android-apps/interfaces.md: the device reports none, so none is made). */
public final class Sensor {
    public static final int TYPE_ACCELEROMETER = 1, TYPE_MAGNETIC_FIELD = 2, TYPE_ORIENTATION = 3, TYPE_GYROSCOPE = 4,
        TYPE_LIGHT = 5, TYPE_PRESSURE = 6, TYPE_PROXIMITY = 8, TYPE_GRAVITY = 9, TYPE_LINEAR_ACCELERATION = 10,
        TYPE_ROTATION_VECTOR = 11, TYPE_RELATIVE_HUMIDITY = 12, TYPE_AMBIENT_TEMPERATURE = 13,
        TYPE_MAGNETIC_FIELD_UNCALIBRATED = 14, TYPE_GAME_ROTATION_VECTOR = 15, TYPE_GYROSCOPE_UNCALIBRATED = 16,
        TYPE_SIGNIFICANT_MOTION = 17, TYPE_STEP_DETECTOR = 18, TYPE_STEP_COUNTER = 19,
        TYPE_GEOMAGNETIC_ROTATION_VECTOR = 20, TYPE_ALL = -1;
    private final int type;
    private final String name, vendor;

    Sensor(int type, String name, String vendor) {
        this.type = type;
        this.name = name;
        this.vendor = vendor;
    }
    public int getType() { return type; }
    public String getName() { return name; }
    public String getVendor() { return vendor; }
    public int getVersion() { return 1; }
    public float getMaximumRange() { return 0; }
    public float getResolution() { return 0; }
    public float getPower() { return 0; }
    public int getMinDelay() { return 0; }
}
