// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** A countdown that calls onTick at each interval and onFinish at the end, on the thread that made it. */
public abstract class CountDownTimer {
    private final long millisInFuture, countdownInterval;
    private long stopTime;
    private boolean cancelled;
    private final Handler handler = new Handler();
    private final Runnable step = this::step;

    public CountDownTimer(long millisInFuture, long countDownInterval) {
        this.millisInFuture = millisInFuture;
        countdownInterval = countDownInterval;
    }

    public final synchronized void cancel() {
        cancelled = true;
        handler.removeCallbacks(step);
    }

    public final synchronized CountDownTimer start() {
        cancelled = false;
        if (millisInFuture <= 0) {
            onFinish();
            return this;
        }
        stopTime = SystemClock.elapsedRealtime() + millisInFuture;
        handler.post(step);
        return this;
    }

    public abstract void onTick(long millisUntilFinished);

    public abstract void onFinish();

    private synchronized void step() {
        if (cancelled) return;
        long left = stopTime - SystemClock.elapsedRealtime();
        if (left <= 0) {
            onFinish();
            return;
        }
        long start = SystemClock.elapsedRealtime();
        onTick(left);
        long spent = SystemClock.elapsedRealtime() - start;
        long delay;
        if (left < countdownInterval) {
            delay = Math.max(left - spent, 0);
        } else {
            delay = countdownInterval - spent;
            while (delay < 0) delay += countdownInterval;
        }
        if (!cancelled) handler.postDelayed(step, delay);
    }
}
