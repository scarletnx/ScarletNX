// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.animation;

/** Interpolates integers. */
public class IntEvaluator implements TypeEvaluator<Integer> {
    @Override public Integer evaluate(float fraction, Integer startValue, Integer endValue) {
        int start = startValue;
        return (int) (start + fraction * (endValue - start));
    }
}
