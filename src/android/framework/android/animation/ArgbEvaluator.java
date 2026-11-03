// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.animation;

/** Colors as ARGB ints, mixed in linear light. */
@SuppressWarnings("rawtypes")
public class ArgbEvaluator implements TypeEvaluator {
    private static final ArgbEvaluator INSTANCE = new ArgbEvaluator();

    public ArgbEvaluator() {}
    public static ArgbEvaluator getInstance() { return INSTANCE; }
    @Override public Object evaluate(float fraction, Object startValue, Object endValue) {
        int a = (Integer) startValue, b = (Integer) endValue, r = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            float x = ((a >>> shift) & 0xff) / 255f, y = ((b >>> shift) & 0xff) / 255f, v;
            if (shift == 24) v = x + fraction * (y - x);
            else v = (float) Math.pow(Math.pow(x, 2.2) + fraction * (Math.pow(y, 2.2) - Math.pow(x, 2.2)), 1 / 2.2);
            r |= Math.round(Math.max(0, Math.min(1, v)) * 255) << shift;
        }
        return r;
    }
}
