// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2010 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.view;

import android.content.Context;
import android.os.Handler;

/**
 * Finds pinch gestures: the span of the pointers around their focus. Quick scale, a double tap and then a vertical
 * drag, scales with one pointer (the mouse): a drag down makes the span larger.
 */
public class ScaleGestureDetector {
    public interface OnScaleGestureListener {
        boolean onScale(ScaleGestureDetector detector);
        boolean onScaleBegin(ScaleGestureDetector detector);
        void onScaleEnd(ScaleGestureDetector detector);
    }

    public static class SimpleOnScaleGestureListener implements OnScaleGestureListener {
        public SimpleOnScaleGestureListener() {}
        @Override public boolean onScale(ScaleGestureDetector detector) { return false; }
        @Override public boolean onScaleBegin(ScaleGestureDetector detector) { return true; }
        @Override public void onScaleEnd(ScaleGestureDetector detector) {}
    }

    private final OnScaleGestureListener listener;
    private final int touchSlop, minSpan, doubleTapSlop;
    private final float density;
    private float focusX, focusY, span, spanX, spanY, prevSpan, prevSpanX, prevSpanY, initialSpan;
    private long time, prevTime, lastUpTime = -1;
    private float lastUpX, lastUpY, anchorX, anchorY;
    private boolean inProgress, quickScaleEnabled = true, stylusScaleEnabled, anchored;

    public ScaleGestureDetector(Context context, OnScaleGestureListener listener) { this(context, listener, null); }
    public ScaleGestureDetector(Context context, OnScaleGestureListener listener, Handler handler) {
        this.listener = listener;
        ViewConfiguration c = ViewConfiguration.get(context);
        touchSlop = c.getScaledTouchSlop() * 2;
        minSpan = c.getScaledMinimumScalingSpan();
        doubleTapSlop = c.getScaledDoubleTapSlop();
        density = snx.android.Device.metrics().density;
    }

    public boolean onTouchEvent(MotionEvent event) {
        time = event.getEventTime();
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN && quickScaleEnabled && lastUpTime >= 0
                && time - lastUpTime <= ViewConfiguration.getDoubleTapTimeout()
                && Math.hypot(event.getX() - lastUpX, event.getY() - lastUpY) < doubleTapSlop) {
            anchored = true;
            anchorX = event.getX();
            anchorY = event.getY();
        }
        boolean streamDone = action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL;
        if (action == MotionEvent.ACTION_UP) {
            lastUpTime = anchored ? -1 : time;
            lastUpX = event.getX();
            lastUpY = event.getY();
        }
        if (action == MotionEvent.ACTION_DOWN || streamDone) {
            if (inProgress) {
                listener.onScaleEnd(this);
                inProgress = false;
                initialSpan = 0;
            }
            if (streamDone) {
                anchored = false;
                return true;
            }
        }
        if (anchored && event.getPointerCount() > 1) anchored = false;
        boolean configChanged = action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_UP || action == MotionEvent.ACTION_POINTER_DOWN;
        int skip = action == MotionEvent.ACTION_POINTER_UP ? event.getActionIndex() : -1;
        float fx = 0, fy = 0, sx = 0, sy = 0, s;
        int count = 0;
        if (anchored) {
            fx = anchorX;
            fy = anchorY;
            s = (float) (minSpan * Math.exp((event.getY() - anchorY) / (100 * density)));
            sx = sy = s;
        } else {
            for (int i = 0; i < event.getPointerCount(); ++i) {
                if (i == skip) continue;
                fx += event.getX(i);
                fy += event.getY(i);
                ++count;
            }
            fx /= Math.max(1, count);
            fy /= Math.max(1, count);
            float dev = 0;
            for (int i = 0; i < event.getPointerCount(); ++i) {
                if (i == skip) continue;
                float dx = event.getX(i) - fx, dy = event.getY(i) - fy;
                sx += Math.abs(dx);
                sy += Math.abs(dy);
                dev += (float) Math.hypot(dx, dy);
            }
            s = dev / Math.max(1, count) * 2;
            sx = sx / Math.max(1, count) * 2;
            sy = sy / Math.max(1, count) * 2;
        }
        boolean wasInProgress = inProgress;
        focusX = fx;
        focusY = fy;
        if (!anchored && inProgress && (s < minSpan || configChanged)) {
            listener.onScaleEnd(this);
            inProgress = false;
            initialSpan = s;
        }
        if (configChanged) {
            prevSpan = span = initialSpan = s;
            prevSpanX = spanX = sx;
            prevSpanY = spanY = sy;
        }
        int minimum = anchored ? touchSlop : minSpan;
        if (!inProgress && s >= minimum && (wasInProgress || Math.abs(s - initialSpan) > touchSlop)) {
            prevSpan = span = s;
            prevSpanX = spanX = sx;
            prevSpanY = spanY = sy;
            prevTime = time;
            inProgress = listener.onScaleBegin(this);
        }
        if (action == MotionEvent.ACTION_MOVE) {
            span = s;
            spanX = sx;
            spanY = sy;
            boolean update = !inProgress || listener.onScale(this);
            if (update) {
                prevSpan = span;
                prevSpanX = spanX;
                prevSpanY = spanY;
                prevTime = time;
            }
        }
        return true;
    }

    public void setQuickScaleEnabled(boolean scales) { quickScaleEnabled = scales; }
    public boolean isQuickScaleEnabled() { return quickScaleEnabled; }
    public void setStylusScaleEnabled(boolean scales) { stylusScaleEnabled = scales; }
    public boolean isStylusScaleEnabled() { return stylusScaleEnabled; }
    public boolean isInProgress() { return inProgress; }
    public float getFocusX() { return focusX; }
    public float getFocusY() { return focusY; }
    public float getCurrentSpan() { return span; }
    public float getCurrentSpanX() { return spanX; }
    public float getCurrentSpanY() { return spanY; }
    public float getPreviousSpan() { return prevSpan; }
    public float getPreviousSpanX() { return prevSpanX; }
    public float getPreviousSpanY() { return prevSpanY; }
    public float getScaleFactor() { return prevSpan > 0 ? span / prevSpan : 1; }
    public long getTimeDelta() { return time - prevTime; }
    public long getEventTime() { return time; }
}
