// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.animation;

/** A rate of change of an animation. */
public interface TimeInterpolator {
    float getInterpolation(float input);
}
