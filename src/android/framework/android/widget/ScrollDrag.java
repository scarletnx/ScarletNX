// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;

/** The drag and fling of a ScrollView or a HorizontalScrollView along its one axis. */
final class ScrollDrag {
    interface Target {
        int position();
        int range();
        void scrollToPosition(int position);
    }

    private final View view;
    private final Target target;
    private final boolean vertical;
    final OverScroller scroller;
    private final int slop, minFling, maxFling;
    private boolean dragging;
    private float down, last, velocity;
    private long lastTime;
    private final Runnable step = this::step;

    ScrollDrag(View view, Target target, boolean vertical) {
        this.view = view;
        this.target = target;
        this.vertical = vertical;
        scroller = new OverScroller(view.getContext());
        ViewConfiguration vc = ViewConfiguration.get(view.getContext());
        slop = vc.getScaledTouchSlop();
        minFling = vc.getScaledMinimumFlingVelocity();
        maxFling = vc.getScaledMaximumFlingVelocity();
    }

    private float pos(MotionEvent ev) { return vertical ? ev.getY() : ev.getX(); }
    private void begin(MotionEvent ev) {
        down = last = pos(ev);
        lastTime = ev.getEventTime();
        velocity = 0;
        dragging = !scroller.isFinished(); // a touch during a fling stops it and drags on
        scroller.abortAnimation();
    }
    private boolean startDrag(MotionEvent ev) {
        if (!dragging && Math.abs(pos(ev) - down) > slop && target.range() > 0) {
            dragging = true;
            last = pos(ev);
            if (view.getParent() != null) view.getParent().requestDisallowInterceptTouchEvent(true);
        }
        return dragging;
    }
    boolean intercept(MotionEvent ev) {
        switch (ev.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: begin(ev); return dragging;
            case MotionEvent.ACTION_MOVE: return startDrag(ev);
            case MotionEvent.ACTION_UP: case MotionEvent.ACTION_CANCEL: dragging = false; return false;
            default: return dragging;
        }
    }
    boolean touch(MotionEvent ev) {
        switch (ev.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: begin(ev); return true;
            case MotionEvent.ACTION_MOVE:
                if (!startDrag(ev)) return true;
                float p = pos(ev), delta = last - p;
                long dt = ev.getEventTime() - lastTime;
                if (dt > 0) velocity = 0.6f * (delta * 1000f / dt) + 0.4f * velocity;
                lastTime = ev.getEventTime();
                int whole = (int) delta;
                if (whole != 0) { target.scrollToPosition(target.position() + whole); last -= whole; }
                return true;
            case MotionEvent.ACTION_UP:
                if (dragging && Math.abs(velocity) > minFling) fling((int) Math.max(-maxFling, Math.min(maxFling, velocity)));
                dragging = false;
                return true;
            case MotionEvent.ACTION_CANCEL: dragging = false; return true;
            default: return true;
        }
    }
    void fling(int velocity) {
        int range = target.range();
        if (range <= 0) return;
        if (vertical) scroller.fling(0, target.position(), 0, velocity, 0, 0, 0, range);
        else scroller.fling(target.position(), 0, velocity, 0, 0, range, 0, 0);
        view.postOnAnimation(step);
    }
    void smoothScrollBy(int delta) {
        int from = target.position(), to = Math.max(0, Math.min(target.range(), from + delta));
        if (to == from) return;
        scroller.abortAnimation();
        if (vertical) scroller.startScroll(0, from, 0, to - from); else scroller.startScroll(from, 0, to - from, 0);
        view.postOnAnimation(step);
    }
    private void step() {
        if (!scroller.computeScrollOffset()) return;
        target.scrollToPosition(vertical ? scroller.getCurrY() : scroller.getCurrX());
        if (!scroller.isFinished()) view.postOnAnimation(step);
    }
}
