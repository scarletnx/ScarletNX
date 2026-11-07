// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.animation;

/** A value of an animation at a fraction of its time. */
public abstract class Keyframe implements Cloneable {
    float fraction;
    Class<?> type;
    boolean hasValue;
    private TimeInterpolator interpolator;

    public static Keyframe ofFloat(float fraction, float value) { return new Simple(fraction, float.class, value); }
    public static Keyframe ofFloat(float fraction) { return new Simple(fraction, float.class, null); }
    public static Keyframe ofInt(float fraction, int value) { return new Simple(fraction, int.class, value); }
    public static Keyframe ofInt(float fraction) { return new Simple(fraction, int.class, null); }
    public static Keyframe ofObject(float fraction, Object value) { return new Simple(fraction, Object.class, value); }
    public static Keyframe ofObject(float fraction) { return new Simple(fraction, Object.class, null); }
    public boolean hasValue() { return hasValue; }
    public abstract Object getValue();
    public abstract void setValue(Object value);
    public float getFraction() { return fraction; }
    public void setFraction(float fraction) { this.fraction = fraction; }
    public TimeInterpolator getInterpolator() { return interpolator; }
    public void setInterpolator(TimeInterpolator interpolator) { this.interpolator = interpolator; }
    public Class getType() { return type; }
    @Override public abstract Keyframe clone();

    private static final class Simple extends Keyframe {
        private Object value;

        Simple(float fraction, Class<?> type, Object value) { this.fraction = fraction; this.type = type; setValue(value); hasValue = value != null; }
        @Override public Object getValue() {
            if (value != null) return value;
            return type == float.class ? (Object) 0f : type == int.class ? (Object) 0 : null;
        }
        @Override public void setValue(Object value) {
            if (value instanceof Number n && type == float.class) value = n.floatValue();
            else if (value instanceof Number n && type == int.class) value = n.intValue();
            this.value = value;
            hasValue = value != null;
        }
        @Override public Keyframe clone() {
            Simple k = new Simple(fraction, type, value);
            k.setInterpolator(getInterpolator());
            return k;
        }
    }
}
