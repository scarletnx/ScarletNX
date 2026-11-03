// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.animation;

/** The value of an animation between two values, at a fraction. */
public interface TypeEvaluator<T> {
    T evaluate(float fraction, T startValue, T endValue);
}
