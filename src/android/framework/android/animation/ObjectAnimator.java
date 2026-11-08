// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.animation;

import android.util.Property;
import java.util.Collections;
import java.util.Set;
import java.util.WeakHashMap;

/** A ValueAnimator that sets properties of a target object on each frame. */
public final class ObjectAnimator extends ValueAnimator {
    private static final Set<ObjectAnimator> autoCancelled = Collections.newSetFromMap(new WeakHashMap<>());
    private Object target;
    private String propertyName;
    private boolean autoCancel;

    public ObjectAnimator() {}
    private ObjectAnimator(Object target, String propertyName) { this.target = target; this.propertyName = propertyName; }

    public static ObjectAnimator ofFloat(Object target, String propertyName, float... values) {
        ObjectAnimator a = new ObjectAnimator(target, propertyName);
        a.setValues(PropertyValuesHolder.ofFloat(propertyName, values));
        return a;
    }
    public static <T> ObjectAnimator ofFloat(T target, Property<T, Float> property, float... values) {
        ObjectAnimator a = new ObjectAnimator(target, property.getName());
        a.setValues(PropertyValuesHolder.ofFloat(property, values));
        return a;
    }
    public static ObjectAnimator ofInt(Object target, String propertyName, int... values) {
        ObjectAnimator a = new ObjectAnimator(target, propertyName);
        a.setValues(PropertyValuesHolder.ofInt(propertyName, values));
        return a;
    }
    public static <T> ObjectAnimator ofInt(T target, Property<T, Integer> property, int... values) {
        ObjectAnimator a = new ObjectAnimator(target, property.getName());
        a.setValues(PropertyValuesHolder.ofInt(property, values));
        return a;
    }
    public static ObjectAnimator ofArgb(Object target, String propertyName, int... values) {
        ObjectAnimator a = ofInt(target, propertyName, values);
        a.setEvaluator(ArgbEvaluator.getInstance());
        return a;
    }
    @SuppressWarnings("rawtypes")
    public static ObjectAnimator ofObject(Object target, String propertyName, TypeEvaluator evaluator, Object... values) {
        ObjectAnimator a = new ObjectAnimator(target, propertyName);
        a.setValues(PropertyValuesHolder.ofObject(propertyName, evaluator, values));
        return a;
    }
    @SafeVarargs
    public static <T, V> ObjectAnimator ofObject(T target, Property<T, V> property, TypeEvaluator<V> evaluator, V... values) {
        ObjectAnimator a = new ObjectAnimator(target, property.getName());
        a.setValues(PropertyValuesHolder.ofObject(property, evaluator, values));
        return a;
    }
    @SafeVarargs
    public static <T, V, P> ObjectAnimator ofObject(T target, Property<T, P> property, TypeConverter<V, P> converter, TypeEvaluator<V> evaluator, V... values) {
        ObjectAnimator a = new ObjectAnimator(target, property.getName());
        a.setValues(PropertyValuesHolder.ofObject(property, converter, evaluator, values));
        return a;
    }
    public static ObjectAnimator ofObject(Object target, String propertyName, TypeConverter<android.graphics.PointF, ?> converter, android.graphics.Path path) {
        ObjectAnimator a = new ObjectAnimator(target, propertyName);
        a.setValues(PropertyValuesHolder.ofObject(propertyName, converter, path));
        return a;
    }
    public static <T, V> ObjectAnimator ofObject(T target, Property<T, V> property, TypeConverter<android.graphics.PointF, V> converter, android.graphics.Path path) {
        ObjectAnimator a = new ObjectAnimator(target, property.getName());
        a.setValues(PropertyValuesHolder.ofObject(property, converter, path));
        return a;
    }
    /** Two float properties along a path, its x and its y. */
    public static ObjectAnimator ofFloat(Object target, String xPropertyName, String yPropertyName, android.graphics.Path path) {
        ObjectAnimator a = new ObjectAnimator(target, null);
        a.setValues(PropertyValuesHolder.ofPathAxis(xPropertyName, null, path, 0), PropertyValuesHolder.ofPathAxis(yPropertyName, null, path, 1));
        return a;
    }
    public static <T> ObjectAnimator ofFloat(T target, Property<T, Float> xProperty, Property<T, Float> yProperty, android.graphics.Path path) {
        ObjectAnimator a = new ObjectAnimator(target, null);
        a.setValues(PropertyValuesHolder.ofPathAxis(null, xProperty, path, 0), PropertyValuesHolder.ofPathAxis(null, yProperty, path, 1));
        return a;
    }
    public static ObjectAnimator ofPropertyValuesHolder(Object target, PropertyValuesHolder... values) {
        ObjectAnimator a = new ObjectAnimator(target, null);
        a.setValues(values);
        return a;
    }

    public void setPropertyName(String propertyName) {
        if (values != null && values.length > 0) values[0].setPropertyName(propertyName);
        this.propertyName = propertyName;
    }
    public void setProperty(Property property) {
        if (values != null && values.length > 0) values[0].setProperty(property);
        propertyName = property.getName();
    }
    public String getPropertyName() {
        if (propertyName != null || values == null || values.length == 0) return propertyName;
        StringBuilder b = new StringBuilder();
        for (PropertyValuesHolder h : values) b.append(b.length() == 0 ? "" : ",").append(h.getPropertyName());
        return b.toString();
    }
    @Override public void setFloatValues(float... v) {
        super.setFloatValues(v);
        if (propertyName != null) values[0].setPropertyName(propertyName);
    }
    @Override public void setIntValues(int... v) {
        super.setIntValues(v);
        if (propertyName != null) values[0].setPropertyName(propertyName);
    }
    @Override public void setObjectValues(Object... v) {
        super.setObjectValues(v);
        if (propertyName != null) values[0].setPropertyName(propertyName);
    }
    public Object getTarget() { return target; }
    @Override public void setTarget(Object target) { if (this.target != target) { if (isStarted()) cancel(); this.target = target; } }
    public void setAutoCancel(boolean cancel) { autoCancel = cancel; }
    @Override public ObjectAnimator setDuration(long duration) { super.setDuration(duration); return this; }
    @Override public void setupStartValues() { if (values != null) for (PropertyValuesHolder h : values) h.setupStart(target); }

    @Override public void start() {
        if (autoCancel) {
            for (ObjectAnimator a : autoCancelled.toArray(new ObjectAnimator[0]))
                if (a != this && a.isRunning() && a.target == target && java.util.Objects.equals(a.getPropertyName(), getPropertyName())) a.cancel();
            autoCancelled.add(this);
        }
        setupStartValues();
        super.start();
    }
    @Override void animateValue(float fraction) {
        if (target == null || values == null) return;
        float t = getInterpolator().getInterpolation(fraction);
        for (PropertyValuesHolder h : values) h.apply(target, h.value(t));
    }
    @Override public ObjectAnimator clone() { return (ObjectAnimator) super.clone(); }
    @Override public String toString() { return "ObjectAnimator{" + getPropertyName() + "}"; }
}
