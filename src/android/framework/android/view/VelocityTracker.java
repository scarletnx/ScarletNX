// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import java.util.ArrayList;
import java.util.HashMap;

/** The velocity of pointers, from a straight-line fit of their last 100 ms of movement. */
public final class VelocityTracker {
    private static final long HORIZON = 100, STOPPED = 40;
    /** For each pointer id: its samples {time, x, y}, oldest first. */
    private final HashMap<Integer, ArrayList<float[]>> samples = new HashMap<>();
    private final HashMap<Integer, float[]> velocities = new HashMap<>();
    private long baseTime = -1;
    private int activePointer = -1;

    private VelocityTracker() {}
    public static VelocityTracker obtain() { return new VelocityTracker(); }
    public void recycle() { clear(); }
    public void clear() { samples.clear(); velocities.clear(); baseTime = -1; activePointer = -1; }

    public void addMovement(MotionEvent event) {
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) clear();
        if (action == MotionEvent.ACTION_POINTER_DOWN) samples.remove(event.getPointerId(event.getActionIndex()));
        long time = event.getEventTime();
        if (baseTime < 0) baseTime = time;
        float t = time - baseTime;
        for (int i = 0; i < event.getPointerCount(); ++i) {
            ArrayList<float[]> list = samples.computeIfAbsent(event.getPointerId(i), k -> new ArrayList<>());
            if (!list.isEmpty() && t - list.get(list.size() - 1)[0] > STOPPED) list.clear();
            list.add(new float[] {t, event.getX(i), event.getY(i)});
            while (list.size() > 1 && t - list.get(0)[0] > HORIZON) list.remove(0);
        }
        int lifting = action == MotionEvent.ACTION_POINTER_UP ? event.getActionIndex() : -1;
        int active = event.findPointerIndex(activePointer);
        if (active < 0 || active == lifting) activePointer = event.getPointerId(lifting == 0 && event.getPointerCount() > 1 ? 1 : 0);
    }

    public void computeCurrentVelocity(int units) { computeCurrentVelocity(units, Float.MAX_VALUE); }
    public void computeCurrentVelocity(int units, float maxVelocity) {
        velocities.clear();
        for (var e : samples.entrySet()) {
            ArrayList<float[]> s = e.getValue();
            float vx = slope(s, 1) * units, vy = slope(s, 2) * units;
            velocities.put(e.getKey(), new float[] {Math.max(-maxVelocity, Math.min(maxVelocity, vx)), Math.max(-maxVelocity, Math.min(maxVelocity, vy))});
        }
    }
    /** The least-squares slope of one coordinate over time, in pixels per millisecond. */
    private static float slope(ArrayList<float[]> s, int axis) {
        int n = s.size();
        if (n < 2) return 0;
        double mt = 0, mv = 0;
        for (float[] p : s) { mt += p[0]; mv += p[axis]; }
        mt /= n;
        mv /= n;
        double num = 0, den = 0;
        for (float[] p : s) { num += (p[0] - mt) * (p[axis] - mv); den += (p[0] - mt) * (p[0] - mt); }
        return den > 0 ? (float) (num / den) : 0;
    }

    public float getXVelocity() { return getXVelocity(activePointer); }
    public float getYVelocity() { return getYVelocity(activePointer); }
    public float getXVelocity(int id) { float[] v = velocities.get(id); return v != null ? v[0] : 0; }
    public float getYVelocity(int id) { float[] v = velocities.get(id); return v != null ? v[1] : 0; }
    public boolean isAxisSupported(int axis) { return axis == MotionEvent.AXIS_X || axis == MotionEvent.AXIS_Y; }
    public float getAxisVelocity(int axis, int id) { return axis == MotionEvent.AXIS_X ? getXVelocity(id) : axis == MotionEvent.AXIS_Y ? getYVelocity(id) : 0; }
    public float getAxisVelocity(int axis) { return getAxisVelocity(axis, activePointer); }
}
