// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.os;

/** Sends messages and runnables to a looper's queue, and handles them on its thread. */
public class Handler {
    public interface Callback {
        boolean handleMessage(Message msg);
    }

    final Looper looper;
    final MessageQueue queue;
    final Callback callback;
    final boolean asynchronous;

    @Deprecated public Handler() { this(null, false); }
    @Deprecated public Handler(Callback callback) { this(callback, false); }
    public Handler(Looper looper) { this(looper, null, false); }
    public Handler(Looper looper, Callback callback) { this(looper, callback, false); }

    public Handler(boolean async) { this(null, async); }

    public Handler(Callback callback, boolean async) {
        looper = Looper.myLooper();
        if (looper == null)
            throw new RuntimeException("Can't create handler inside thread " + Thread.currentThread() + " that has not called Looper.prepare()");
        queue = looper.queue;
        this.callback = callback;
        asynchronous = async;
    }

    public Handler(Looper looper, Callback callback, boolean async) {
        this.looper = looper;
        queue = looper.queue;
        this.callback = callback;
        asynchronous = async;
    }

    public static Handler createAsync(Looper looper) {
        if (looper == null) throw new NullPointerException("looper must not be null");
        return new Handler(looper, null, true);
    }

    public static Handler createAsync(Looper looper, Callback callback) {
        return new Handler(looper, callback, true);
    }

    public static Handler getMain() {
        return new Handler(Looper.getMainLooper());
    }

    public static Handler mainIfNull(Handler handler) {
        return handler == null ? getMain() : handler;
    }

    public void handleMessage(Message msg) {}

    public void dispatchMessage(Message msg) {
        if (msg.callback != null) {
            msg.callback.run();
        } else {
            if (callback != null && callback.handleMessage(msg)) return;
            handleMessage(msg);
        }
    }

    public String getMessageName(Message message) {
        return message.callback != null ? message.callback.getClass().getName() : "0x" + Integer.toHexString(message.what);
    }

    public final Message obtainMessage() { return Message.obtain(this); }
    public final Message obtainMessage(int what) { return Message.obtain(this, what); }
    public final Message obtainMessage(int what, Object obj) { return Message.obtain(this, what, obj); }
    public final Message obtainMessage(int what, int arg1, int arg2) { return Message.obtain(this, what, arg1, arg2); }
    public final Message obtainMessage(int what, int arg1, int arg2, Object obj) {
        return Message.obtain(this, what, arg1, arg2, obj);
    }

    private static Message runnable(Runnable r) {
        Message m = Message.obtain();
        m.callback = r;
        return m;
    }

    private static Message runnable(Runnable r, Object token) {
        Message m = runnable(r);
        m.obj = token;
        return m;
    }

    public final boolean post(Runnable r) { return sendMessageDelayed(runnable(r), 0); }
    public final boolean postAtTime(Runnable r, long uptimeMillis) { return sendMessageAtTime(runnable(r), uptimeMillis); }
    public final boolean postAtTime(Runnable r, Object token, long uptimeMillis) {
        return sendMessageAtTime(runnable(r, token), uptimeMillis);
    }
    public final boolean postDelayed(Runnable r, long delayMillis) { return sendMessageDelayed(runnable(r), delayMillis); }
    public final boolean postDelayed(Runnable r, Object token, long delayMillis) {
        return sendMessageDelayed(runnable(r, token), delayMillis);
    }
    public final boolean postAtFrontOfQueue(Runnable r) { return sendMessageAtFrontOfQueue(runnable(r)); }

    public final boolean runWithScissors(Runnable r, long timeout) {
        if (Looper.myLooper() == looper) {
            r.run();
            return true;
        }
        final boolean[] done = {false};
        post(() -> {
            try { r.run(); } finally { synchronized (done) { done[0] = true; done.notifyAll(); } }
        });
        synchronized (done) {
            long end = SystemClock.uptimeMillis() + timeout;
            while (!done[0]) {
                long wait = timeout > 0 ? end - SystemClock.uptimeMillis() : 0;
                if (timeout > 0 && wait <= 0) return false;
                try { done.wait(Math.max(wait, 0)); } catch (InterruptedException e) { }
            }
        }
        return true;
    }

    public final void removeCallbacks(Runnable r) { queue.removeMessages(this, r, null); }
    public final void removeCallbacks(Runnable r, Object token) { queue.removeMessages(this, r, token); }

    public final boolean sendMessage(Message msg) { return sendMessageDelayed(msg, 0); }
    public final boolean sendEmptyMessage(int what) { return sendEmptyMessageDelayed(what, 0); }
    public final boolean sendEmptyMessageDelayed(int what, long delayMillis) {
        Message m = Message.obtain();
        m.what = what;
        return sendMessageDelayed(m, delayMillis);
    }
    public final boolean sendEmptyMessageAtTime(int what, long uptimeMillis) {
        Message m = Message.obtain();
        m.what = what;
        return sendMessageAtTime(m, uptimeMillis);
    }
    public final boolean sendMessageDelayed(Message msg, long delayMillis) {
        if (delayMillis < 0) delayMillis = 0;
        return sendMessageAtTime(msg, SystemClock.uptimeMillis() + delayMillis);
    }
    public boolean sendMessageAtTime(Message msg, long uptimeMillis) {
        msg.target = this;
        if (asynchronous) msg.asynchronous = true;
        return queue.enqueueMessage(msg, uptimeMillis);
    }
    public final boolean sendMessageAtFrontOfQueue(Message msg) {
        msg.target = this;
        if (asynchronous) msg.asynchronous = true;
        return queue.enqueueMessage(msg, 0);
    }

    public final void removeMessages(int what) { queue.removeMessages(this, what, null); }
    public final void removeMessages(int what, Object object) { queue.removeMessages(this, what, object); }
    public final void removeCallbacksAndMessages(Object token) { queue.removeCallbacksAndMessages(this, token); }
    public final boolean hasMessages(int what) { return queue.hasMessages(this, what, null); }
    public final boolean hasMessages(int what, Object object) { return queue.hasMessages(this, what, object); }
    public final boolean hasCallbacks(Runnable r) { return queue.hasMessages(this, r, null); }
    public final boolean hasMessagesOrCallbacks() { return queue.hasMessages(this); }
    public final Looper getLooper() { return looper; }

    @Override
    public String toString() {
        return "Handler (" + getClass().getName() + ") {" + Integer.toHexString(System.identityHashCode(this)) + "}";
    }
}
