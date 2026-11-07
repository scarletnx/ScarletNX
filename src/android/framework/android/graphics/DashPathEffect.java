// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics;

/** Dashes along a stroke. */
public class DashPathEffect extends PathEffect {
    public DashPathEffect(float[] intervals, float phase) {
        if (intervals.length < 2 || (intervals.length & 1) != 0) throw new ArrayIndexOutOfBoundsException();
        this.intervals = intervals.clone();
        this.phase = phase;
    }
}
