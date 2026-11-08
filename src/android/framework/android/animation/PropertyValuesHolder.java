// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.animation;

import android.util.Property;
import java.lang.reflect.Method;

/** A property and the keyframes of its values, for an animator. */
@SuppressWarnings({"rawtypes", "unchecked"})
public class PropertyValuesHolder implements Cloneable {
    private String propertyName;
    private Property property;
    private Keyframe[] keyframes = new Keyframe[0];
    private TypeEvaluator evaluator;
    private Method setter, getter;
    private Class<?> setterClass;

    private PropertyValuesHolder(String propertyName) { this.propertyName = propertyName; }
    private PropertyValuesHolder(Property property) { this.property = property; if (property != null) propertyName = property.getName(); }

    public static PropertyValuesHolder ofFloat(String propertyName, float... values) { PropertyValuesHolder h = new PropertyValuesHolder(propertyName); h.setFloatValues(values); return h; }
    public static PropertyValuesHolder ofFloat(Property<?, Float> property, float... values) { PropertyValuesHolder h = new PropertyValuesHolder(property); h.setFloatValues(values); return h; }
    public static PropertyValuesHolder ofInt(String propertyName, int... values) { PropertyValuesHolder h = new PropertyValuesHolder(propertyName); h.setIntValues(values); return h; }
    public static PropertyValuesHolder ofInt(Property<?, Integer> property, int... values) { PropertyValuesHolder h = new PropertyValuesHolder(property); h.setIntValues(values); return h; }
    public static PropertyValuesHolder ofObject(String propertyName, TypeEvaluator evaluator, Object... values) {
        PropertyValuesHolder h = new PropertyValuesHolder(propertyName);
        h.setObjectValues(values);
        h.setEvaluator(evaluator);
        return h;
    }
    public static PropertyValuesHolder ofKeyframe(String propertyName, Keyframe... values) {
        PropertyValuesHolder h = new PropertyValuesHolder(propertyName);
        h.setKeyframes(values);
        return h;
    }

    public void setFloatValues(float... values) { frames(values.length, i -> Keyframe.ofFloat(0, values[i]), Keyframe.ofFloat(0)); }
    public void setIntValues(int... values) { frames(values.length, i -> Keyframe.ofInt(0, values[i]), Keyframe.ofInt(0)); }
    public void setObjectValues(Object... values) { frames(values.length, i -> Keyframe.ofObject(0, values[i]), Keyframe.ofObject(0)); }
    public void setKeyframes(Keyframe... values) {
        keyframes = new Keyframe[values.length];
        for (int i = 0; i < values.length; ++i) keyframes[i] = values[i].clone();
    }
    /** Values spread evenly over the time. One value is the end, and the target's current value is the start. */
    private void frames(int n, java.util.function.IntFunction<Keyframe> make, Keyframe start) {
        keyframes = new Keyframe[n == 1 ? 2 : n];
        for (int i = 0; i < n; ++i) {
            Keyframe k = make.apply(i);
            k.setFraction(n == 1 ? 1 : i / (float) (n - 1));
            keyframes[keyframes.length - n + i] = k;
        }
        if (n == 1) keyframes[0] = start;
    }
    public void setEvaluator(TypeEvaluator evaluator) { this.evaluator = evaluator; }
    public void setPropertyName(String propertyName) { this.propertyName = propertyName; setter = getter = null; }
    public String getPropertyName() { return propertyName; }
    public void setProperty(Property property) { this.property = property; }

    /** The value at an interpolated fraction, between the keyframes around it. */
    Object value(float t) {
        Keyframe[] k = keyframes;
        if (k.length == 0) return null;
        if (k.length == 1) return k[0].getValue();
        int i = 1;
        while (i < k.length - 1 && t > k[i].getFraction()) ++i;
        Keyframe a = k[i - 1], b = k[i];
        float span = b.getFraction() - a.getFraction();
        float f = span == 0 ? 1 : (t - a.getFraction()) / span;
        if (b.getInterpolator() != null) f = b.getInterpolator().getInterpolation(f);
        Object x = a.getValue(), y = b.getValue();
        if (evaluator != null) return evaluator.evaluate(f, x, y);
        if (x instanceof Integer p && y instanceof Integer q) return (int) (p + f * (q - p));
        if (x instanceof Number p && y instanceof Number q) return p.floatValue() + f * (q.floatValue() - p.floatValue());
        return f < 1 ? x : y;
    }

    /** Fills the keyframes without a value from the target's current value. */
    void setupStart(Object target) {
        if (target == null) return;
        for (Keyframe k : keyframes) {
            if (k.hasValue()) continue;
            Object v = null;
            try {
                if (property != null) v = property.get(target);
                else {
                    if (getter == null) getter = target.getClass().getMethod("get" + capital(propertyName));
                    v = getter.invoke(target);
                }
            } catch (ReflectiveOperationException | RuntimeException e) {
                // No getter: the keyframe keeps its default value.
            }
            if (v != null) k.setValue(v);
        }
    }

    /** Sets the target's property to a value. */
    void apply(Object target, Object value) {
        if (target == null || value == null) return;
        if (property != null) { property.set(target, value); return; }
        try {
            if (setter == null || setterClass != target.getClass()) {
                setter = null;
                setterClass = target.getClass();
                String name = "set" + capital(propertyName);
                for (Method m : setterClass.getMethods()) {
                    if (!m.getName().equals(name) || m.getParameterCount() != 1) continue;
                    Class<?> p = m.getParameterTypes()[0];
                    if (setter == null || (value instanceof Float && p == float.class) || (value instanceof Integer && p == int.class)) setter = m;
                }
                if (setter == null) return;
            }
            Class<?> p = setter.getParameterTypes()[0];
            if (p == float.class && value instanceof Number n) value = n.floatValue();
            else if (p == int.class && value instanceof Number n) value = n.intValue();
            setter.invoke(target, value);
        } catch (ReflectiveOperationException | RuntimeException e) {
            // A property without a setter is not animated.
        }
    }
    private static String capital(String s) { return s == null || s.isEmpty() ? "" : Character.toUpperCase(s.charAt(0)) + s.substring(1); }

    @Override public PropertyValuesHolder clone() {
        try {
            PropertyValuesHolder h = (PropertyValuesHolder) super.clone();
            h.setKeyframes(keyframes);
            return h;
        } catch (CloneNotSupportedException e) {
            throw new AssertionError(e);
        }
    }
    @Override public String toString() { return propertyName + ": " + keyframes.length + " keyframes"; }
}
