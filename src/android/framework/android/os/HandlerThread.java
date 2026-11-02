// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** A thread with a looper. */
public class HandlerThread extends Thread {
    int priority;
    Looper looper;
    private Handler handler;

    public HandlerThread(String name) {
        this(name, Process.THREAD_PRIORITY_DEFAULT);
    }

    public HandlerThread(String name, int priority) {
        super(name);
        this.priority = priority;
        setDaemon(true);
    }

    protected void onLooperPrepared() {}

    @Override
    public void run() {
        Looper.prepare();
        synchronized (this) {
            looper = Looper.myLooper();
            notifyAll();
        }
        onLooperPrepared();
        Looper.loop();
    }

    public Looper getLooper() {
        if (!isAlive()) return null;
        synchronized (this) {
            while (isAlive() && looper == null) {
                try { wait(); } catch (InterruptedException e) { }
            }
        }
        return looper;
    }

    public Handler getThreadHandler() {
        if (handler == null) handler = new Handler(getLooper());
        return handler;
    }

    public boolean quit() {
        Looper l = getLooper();
        if (l == null) return false;
        l.quit();
        return true;
    }

    public boolean quitSafely() {
        Looper l = getLooper();
        if (l == null) return false;
        l.quitSafely();
        return true;
    }

    public int getThreadId() {
        return (int) threadId();
    }
}
