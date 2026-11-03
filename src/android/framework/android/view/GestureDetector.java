// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;

/** Finds taps, double taps, long presses, scrolls and flings in a stream of touch events. */
public class GestureDetector {
    public interface OnGestureListener {
        boolean onDown(MotionEvent e);
        void onShowPress(MotionEvent e);
        boolean onSingleTapUp(MotionEvent e);
        boolean onScroll(MotionEvent e1, MotionEvent e2, float distanceX, float distanceY);
        void onLongPress(MotionEvent e);
        boolean onFling(MotionEvent e1, MotionEvent e2, float velocityX, float velocityY);
    }

    public interface OnDoubleTapListener {
        boolean onSingleTapConfirmed(MotionEvent e);
        boolean onDoubleTap(MotionEvent e);
        boolean onDoubleTapEvent(MotionEvent e);
    }

    public interface OnContextClickListener {
        boolean onContextClick(MotionEvent e);
    }

    public static class SimpleOnGestureListener implements OnGestureListener, OnDoubleTapListener, OnContextClickListener {
        public SimpleOnGestureListener() {}
        @Override public boolean onSingleTapUp(MotionEvent e) { return false; }
        @Override public void onLongPress(MotionEvent e) {}
        @Override public boolean onScroll(MotionEvent e1, MotionEvent e2, float distanceX, float distanceY) { return false; }
        @Override public boolean onFling(MotionEvent e1, MotionEvent e2, float velocityX, float velocityY) { return false; }
        @Override public void onShowPress(MotionEvent e) {}
        @Override public boolean onDown(MotionEvent e) { return false; }
        @Override public boolean onDoubleTap(MotionEvent e) { return false; }
        @Override public boolean onDoubleTapEvent(MotionEvent e) { return false; }
        @Override public boolean onSingleTapConfirmed(MotionEvent e) { return false; }
        @Override public boolean onContextClick(MotionEvent e) { return false; }
    }

    private static final int DOUBLE_TAP_MIN_TIME = 40;
    private final OnGestureListener listener;
    private OnDoubleTapListener doubleTapListener;
    private OnContextClickListener contextClickListener;
    private final Handler handler;
    private final int touchSlopSquare, doubleTapTouchSlopSquare, doubleTapSlopSquare, minFling, maxFling;
    private boolean longpressEnabled = true, inLongPress, stillDown, deferConfirmSingleTap, tapPending;
    private boolean alwaysInTapRegion, alwaysInBiggerTapRegion, isDoubleTapping;
    private MotionEvent currentDown, previousUp;
    private float lastX, lastY, downX, downY;
    private VelocityTracker velocity;
    private final Runnable showPress = this::showPress;
    private final Runnable longPress = this::longPress;
    private final Runnable tap = this::tap;

    @Deprecated public GestureDetector(OnGestureListener listener, Handler handler) { this(null, listener, handler); }
    @Deprecated public GestureDetector(OnGestureListener listener) { this(null, listener, null); }
    public GestureDetector(Context context, OnGestureListener listener) { this(context, listener, null); }
    public GestureDetector(Context context, OnGestureListener listener, Handler handler) {
        if (listener == null) throw new NullPointerException("OnGestureListener must not be null");
        this.listener = listener;
        this.handler = handler != null ? new Handler(handler.getLooper()) : new Handler(Looper.myLooper() != null ? Looper.myLooper() : Looper.getMainLooper());
        if (listener instanceof OnDoubleTapListener d) doubleTapListener = d;
        if (listener instanceof OnContextClickListener c) contextClickListener = c;
        int touchSlop, doubleTapSlop;
        if (context == null) {
            touchSlop = ViewConfiguration.getTouchSlop();
            doubleTapSlop = 100;
            minFling = 50;
            maxFling = 8000;
        } else {
            ViewConfiguration c = ViewConfiguration.get(context);
            touchSlop = c.getScaledTouchSlop();
            doubleTapSlop = c.getScaledDoubleTapSlop();
            minFling = c.getScaledMinimumFlingVelocity();
            maxFling = c.getScaledMaximumFlingVelocity();
        }
        touchSlopSquare = touchSlop * touchSlop;
        doubleTapTouchSlopSquare = touchSlopSquare;
        doubleTapSlopSquare = doubleTapSlop * doubleTapSlop;
    }
    public GestureDetector(Context context, OnGestureListener listener, Handler handler, boolean unused) { this(context, listener, handler); }

    public void setOnDoubleTapListener(OnDoubleTapListener onDoubleTapListener) { doubleTapListener = onDoubleTapListener; }
    public void setContextClickListener(OnContextClickListener onContextClickListener) { contextClickListener = onContextClickListener; }
    public void setIsLongpressEnabled(boolean isLongpressEnabled) { longpressEnabled = isLongpressEnabled; }
    public boolean isLongpressEnabled() { return longpressEnabled; }

    public boolean onTouchEvent(MotionEvent ev) {
        if (velocity == null) velocity = VelocityTracker.obtain();
        velocity.addMovement(ev);
        int action = ev.getActionMasked();
        int skip = action == MotionEvent.ACTION_POINTER_UP ? ev.getActionIndex() : -1;
        float sumX = 0, sumY = 0;
        int count = 0;
        for (int i = 0; i < ev.getPointerCount(); ++i) {
            if (i == skip) continue;
            sumX += ev.getX(i);
            sumY += ev.getY(i);
            ++count;
        }
        float focusX = count > 0 ? sumX / count : ev.getX(), focusY = count > 0 ? sumY / count : ev.getY();
        boolean handled = false;
        switch (action) {
            case MotionEvent.ACTION_POINTER_DOWN:
            case MotionEvent.ACTION_POINTER_UP:
                downX = lastX = focusX;
                downY = lastY = focusY;
                cancelTaps();
                break;
            case MotionEvent.ACTION_DOWN:
                if (doubleTapListener != null) {
                    boolean hadTap = tapPending;
                    handler.removeCallbacks(tap);
                    tapPending = false;
                    if (currentDown != null && previousUp != null && hadTap && isDoubleTap(currentDown, previousUp, ev)) {
                        isDoubleTapping = true;
                        handled |= doubleTapListener.onDoubleTap(currentDown);
                        handled |= doubleTapListener.onDoubleTapEvent(ev);
                    } else {
                        tapPending = true;
                        handler.postDelayed(tap, ViewConfiguration.getDoubleTapTimeout());
                    }
                }
                downX = lastX = focusX;
                downY = lastY = focusY;
                currentDown = MotionEvent.obtain(ev);
                alwaysInTapRegion = alwaysInBiggerTapRegion = true;
                stillDown = true;
                inLongPress = false;
                deferConfirmSingleTap = false;
                handler.removeCallbacks(longPress);
                if (longpressEnabled) handler.postDelayed(longPress, ViewConfiguration.getTapTimeout() + ViewConfiguration.getLongPressTimeout());
                handler.removeCallbacks(showPress);
                handler.postDelayed(showPress, ViewConfiguration.getTapTimeout());
                handled |= listener.onDown(ev);
                break;
            case MotionEvent.ACTION_MOVE: {
                if (inLongPress) break;
                float scrollX = lastX - focusX, scrollY = lastY - focusY;
                if (isDoubleTapping) {
                    handled |= doubleTapListener.onDoubleTapEvent(ev);
                } else if (alwaysInTapRegion) {
                    float dx = focusX - downX, dy = focusY - downY, distance = dx * dx + dy * dy;
                    if (distance > touchSlopSquare) {
                        handled = listener.onScroll(currentDown, ev, scrollX, scrollY);
                        lastX = focusX;
                        lastY = focusY;
                        alwaysInTapRegion = false;
                        handler.removeCallbacks(tap);
                        tapPending = false;
                        handler.removeCallbacks(showPress);
                        handler.removeCallbacks(longPress);
                    }
                    if (distance > doubleTapTouchSlopSquare) alwaysInBiggerTapRegion = false;
                } else if (Math.abs(scrollX) >= 1 || Math.abs(scrollY) >= 1) {
                    handled = listener.onScroll(currentDown, ev, scrollX, scrollY);
                    lastX = focusX;
                    lastY = focusY;
                }
                break;
            }
            case MotionEvent.ACTION_UP: {
                stillDown = false;
                MotionEvent up = MotionEvent.obtain(ev);
                if (isDoubleTapping) {
                    handled |= doubleTapListener.onDoubleTapEvent(ev);
                } else if (inLongPress) {
                    handler.removeCallbacks(tap);
                    tapPending = false;
                    inLongPress = false;
                } else if (alwaysInTapRegion) {
                    handled = listener.onSingleTapUp(ev);
                    if (deferConfirmSingleTap && doubleTapListener != null) doubleTapListener.onSingleTapConfirmed(ev);
                } else {
                    int id = ev.getPointerId(0);
                    velocity.computeCurrentVelocity(1000, maxFling);
                    float vx = velocity.getXVelocity(id), vy = velocity.getYVelocity(id);
                    if (Math.abs(vx) > minFling || Math.abs(vy) > minFling) handled = listener.onFling(currentDown, ev, vx, vy);
                }
                previousUp = up;
                velocity.recycle();
                velocity = null;
                isDoubleTapping = false;
                deferConfirmSingleTap = false;
                handler.removeCallbacks(showPress);
                handler.removeCallbacks(longPress);
                break;
            }
            case MotionEvent.ACTION_CANCEL:
                cancel();
                break;
        }
        return handled;
    }

    public boolean onGenericMotionEvent(MotionEvent ev) {
        int a = ev.getActionMasked();
        if (contextClickListener != null && a == MotionEvent.ACTION_BUTTON_PRESS
                && (ev.getActionButton() == MotionEvent.BUTTON_SECONDARY || ev.getActionButton() == MotionEvent.BUTTON_STYLUS_PRIMARY)
                && contextClickListener.onContextClick(ev)) {
            handler.removeCallbacks(longPress);
            return true;
        }
        return false;
    }

    private boolean isDoubleTap(MotionEvent firstDown, MotionEvent firstUp, MotionEvent secondDown) {
        if (!alwaysInBiggerTapRegion) return false;
        long dt = secondDown.getEventTime() - firstUp.getEventTime();
        if (dt > ViewConfiguration.getDoubleTapTimeout() || dt < DOUBLE_TAP_MIN_TIME) return false;
        float dx = firstDown.getX() - secondDown.getX(), dy = firstDown.getY() - secondDown.getY();
        return dx * dx + dy * dy < doubleTapSlopSquare;
    }
    private void showPress() { listener.onShowPress(currentDown); }
    private void tap() {
        tapPending = false;
        if (doubleTapListener == null) return;
        if (!stillDown) doubleTapListener.onSingleTapConfirmed(currentDown);
        else deferConfirmSingleTap = true;
    }
    private void longPress() {
        handler.removeCallbacks(tap);
        tapPending = false;
        deferConfirmSingleTap = false;
        inLongPress = true;
        listener.onLongPress(currentDown);
    }
    private void cancelTaps() {
        handler.removeCallbacks(showPress);
        handler.removeCallbacks(longPress);
        handler.removeCallbacks(tap);
        tapPending = false;
        isDoubleTapping = false;
        alwaysInTapRegion = alwaysInBiggerTapRegion = false;
        deferConfirmSingleTap = false;
        inLongPress = false;
    }
    private void cancel() {
        cancelTaps();
        if (velocity != null) { velocity.recycle(); velocity = null; }
        stillDown = false;
    }
}
