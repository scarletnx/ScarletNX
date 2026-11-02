// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.hardware;

/** A sensor's reading */
public class SensorEvent {
    public final float[] values;
    public Sensor sensor;
    public int accuracy;
    public long timestamp;

    SensorEvent(int valueSize) { values = new float[valueSize]; }
}
