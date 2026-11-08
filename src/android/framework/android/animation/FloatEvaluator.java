// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.animation;

/** Interpolates numbers as floats. */
public class FloatEvaluator implements TypeEvaluator<Number> {
    @Override public Float evaluate(float fraction, Number startValue, Number endValue) {
        float start = startValue.floatValue();
        return start + fraction * (endValue.floatValue() - start);
    }
}
