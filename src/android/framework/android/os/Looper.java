// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.os;

/** A thread's message loop. */
public final class Looper {
    static final ThreadLocal<Looper> threadLocal = new ThreadLocal<>();
    private static Looper mainLooper;
    final MessageQueue queue;
    final Thread thread;
    private android.util.Printer logging;

    private Looper(boolean quitAllowed) {
        queue = new MessageQueue(quitAllowed);
        thread = Thread.currentThread();
    }

    public static void prepare() {
        prepare(true);
    }

    private static void prepare(boolean quitAllowed) {
        if (threadLocal.get() != null) throw new RuntimeException("Only one Looper may be created per thread");
        threadLocal.set(new Looper(quitAllowed));
    }

    @Deprecated
    public static void prepareMainLooper() {
        prepare(false);
        synchronized (Looper.class) {
            if (mainLooper != null) throw new IllegalStateException("The main Looper has already been prepared.");
            mainLooper = myLooper();
        }
    }

    public static Looper getMainLooper() {
        synchronized (Looper.class) {
            return mainLooper;
        }
    }

    /** Dispatches the messages of the thread's queue until the looper quits. */
    public static void loop() {
        Looper me = myLooper();
        if (me == null) throw new RuntimeException("No Looper; Looper.prepare() wasn't called on this thread.");
        for (;;) {
            if (!loopOnce(me)) return;
        }
    }

    /** One message dispatched; false when the looper quit. */
    public static boolean loopOnce(Looper me) {
        Message msg = me.queue.next();
        if (msg == null) return false;
        android.util.Printer log = me.logging;
        if (log != null) log.println(">>>>> Dispatching to " + msg.target + " " + msg.callback + ": " + msg.what);
        msg.target.dispatchMessage(msg);
        if (log != null) log.println("<<<<< Finished to " + msg.target + " " + msg.callback);
        return true;
    }

    public static Looper myLooper() {
        return threadLocal.get();
    }

    public static MessageQueue myQueue() {
        return myLooper().queue;
    }

    public boolean isCurrentThread() {
        return Thread.currentThread() == thread;
    }

    public void setMessageLogging(android.util.Printer printer) {
        logging = printer;
    }

    public void quit() {
        queue.quit(false);
    }

    public void quitSafely() {
        queue.quit(true);
    }

    public Thread getThread() {
        return thread;
    }

    public MessageQueue getQueue() {
        return queue;
    }

    @Override
    public String toString() {
        return "Looper (" + thread.getName() + ", tid " + thread.threadId() + ") {" + Integer.toHexString(System.identityHashCode(this)) + "}";
    }
}
