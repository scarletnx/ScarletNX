// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** A condition that threads block on until another thread opens it. */
public class ConditionVariable {
    private volatile boolean condition;

    public ConditionVariable() {}
    public ConditionVariable(boolean state) { condition = state; }

    public synchronized void open() {
        boolean old = condition;
        condition = true;
        if (!old) notifyAll();
    }

    public synchronized void close() { condition = false; }

    public synchronized void block() {
        while (!condition) {
            try { wait(); } catch (InterruptedException e) {}
        }
    }

    public synchronized boolean block(long timeoutMs) {
        if (timeoutMs == 0) { block(); return true; }
        long end = SystemClock.elapsedRealtime() + timeoutMs;
        while (!condition) {
            long left = end - SystemClock.elapsedRealtime();
            if (left <= 0) break;
            try { wait(left); } catch (InterruptedException e) {}
        }
        return condition;
    }
}
