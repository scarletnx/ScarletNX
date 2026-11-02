// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import java.util.ArrayList;

/**
 * Frames: input, animation, traversal and commit callbacks, in that order, once for each vsync. The vsync is the
 * window's present pace (snx.android.ui.Window drives frames); without a window, a 60 Hz timer.
 */
public final class Choreographer {
    public static final int CALLBACK_INPUT = 0, CALLBACK_ANIMATION = 1, CALLBACK_INSETS_ANIMATION = 2, CALLBACK_TRAVERSAL = 3,
        CALLBACK_COMMIT = 4;
    private static final int TYPES = 5;
    private static final long FRAME_NANOS = 16_666_667L;
    private static final ThreadLocal<Choreographer> instance = ThreadLocal.withInitial(() -> {
        Looper looper = Looper.myLooper();
        if (looper == null) throw new IllegalStateException("The current thread must have a looper!");
        return new Choreographer(looper);
    });

    public interface FrameCallback {
        void doFrame(long frameTimeNanos);
    }

    public interface VsyncCallback {
        void onVsync(FrameData data);
    }

    public static class FrameData {
        private final long frameTimeNanos;
        FrameData(long frameTimeNanos) { this.frameTimeNanos = frameTimeNanos; }
        public long getFrameTimeNanos() { return frameTimeNanos; }
    }

    private record Callback(Object action, Object token, long dueTime) {}

    private final Handler handler;
    private final ArrayList<ArrayList<Callback>> queues = new ArrayList<>();
    private boolean scheduled;
    private long lastFrameNanos, frameNanos;
    private static Runnable vsyncRequest;

    private Choreographer(Looper looper) {
        handler = new Handler(looper, null, true);
        for (int i = 0; i < TYPES; ++i) queues.add(new ArrayList<>());
    }

    public static Choreographer getInstance() { return instance.get(); }
    public static Choreographer getMainThreadInstance() { return instance.get(); }
    public static long getFrameDelay() { return 10; }
    public static void setFrameDelay(long frameDelay) {}

    /** How the window asks for the next vsync: it calls doFrame then. Null for the timer. */
    public static void setVsyncSource(Runnable request) { vsyncRequest = request; }

    public long getFrameTime() { return getFrameTimeNanos() / 1000000; }
    public long getFrameTimeNanos() { return frameNanos != 0 ? frameNanos : System.nanoTime(); }
    public long getLastFrameTimeNanos() { return lastFrameNanos; }
    public long getFrameIntervalNanos() { return FRAME_NANOS; }

    public void postFrameCallback(FrameCallback callback) { postFrameCallbackDelayed(callback, 0); }
    public void postFrameCallbackDelayed(FrameCallback callback, long delayMillis) {
        if (callback == null) throw new IllegalArgumentException("callback must not be null");
        post(CALLBACK_ANIMATION, callback, FRAME_CALLBACK_TOKEN, delayMillis);
    }
    public void removeFrameCallback(FrameCallback callback) { remove(CALLBACK_ANIMATION, callback, FRAME_CALLBACK_TOKEN); }
    public void postCallback(int callbackType, Runnable action, Object token) { post(callbackType, action, token, 0); }
    public void postCallbackDelayed(int callbackType, Runnable action, Object token, long delayMillis) { post(callbackType, action, token, delayMillis); }
    public void removeCallbacks(int callbackType, Runnable action, Object token) { remove(callbackType, action, token); }
    public void postVsyncCallback(VsyncCallback callback) { postFrameCallback(t -> callback.onVsync(new FrameData(t))); }
    public void removeVsyncCallback(VsyncCallback callback) {}
    private static final Object FRAME_CALLBACK_TOKEN = new Object();

    private void post(int type, Object action, Object token, long delayMillis) {
        long due = SystemClock.uptimeMillis() + Math.max(0, delayMillis);
        synchronized (this) {
            queues.get(type).add(new Callback(action, token, due));
            if (delayMillis <= 0) schedule();
            else handler.postDelayed(this::schedule, delayMillis);
        }
    }

    private void remove(int type, Object action, Object token) {
        synchronized (this) {
            queues.get(type).removeIf(c -> (action == null || c.action == action) && (token == null || c.token == token));
        }
    }

    private void schedule() {
        synchronized (this) {
            if (scheduled) return;
            scheduled = true;
        }
        Runnable request = vsyncRequest;
        if (request != null && handler.getLooper() == Looper.getMainLooper()) {
            request.run();
        } else {
            long now = System.nanoTime();
            long next = lastFrameNanos == 0 ? now : Math.max(now, lastFrameNanos + FRAME_NANOS);
            Runnable frame = () -> doFrame(System.nanoTime());
            handler.postDelayed(frame, (next - now) / 1000000);
        }
    }

    /** A frame: the callbacks due now, by type. The window calls it at each vsync it was asked for. */
    public void doFrame(long frameTimeNanos) {
        synchronized (this) {
            if (!scheduled) return;
            scheduled = false;
        }
        lastFrameNanos = frameTimeNanos;
        frameNanos = frameTimeNanos;
        long now = SystemClock.uptimeMillis();
        try {
            for (int type = 0; type < TYPES; ++type) {
                ArrayList<Callback> due = new ArrayList<>();
                synchronized (this) {
                    ArrayList<Callback> q = queues.get(type);
                    for (int i = 0; i < q.size(); ++i) if (q.get(i).dueTime <= now) due.add(q.remove(i--));
                }
                for (Callback c : due) {
                    if (c.action instanceof FrameCallback f) f.doFrame(frameTimeNanos);
                    else ((Runnable) c.action).run();
                }
            }
        } finally {
            frameNanos = 0;
        }
    }
}
