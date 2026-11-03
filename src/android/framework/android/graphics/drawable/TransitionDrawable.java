// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.drawable;

import android.graphics.Canvas;
import android.os.SystemClock;

/** Two layers, with a fade from the first to the second, or back. */
public class TransitionDrawable extends LayerDrawable implements Drawable.Callback {
    private static final int IDLE = 0, STARTING = 1, RUNNING = 2;
    private int transition = IDLE, from, to, duration, originalDuration, alpha;
    private long startTime;
    private boolean reverse, crossFade;

    public TransitionDrawable(Drawable[] layers) {
        super(layers);
        if (layers.length < 2) throw new IllegalArgumentException("A TransitionDrawable needs at least two layers");
    }

    public void startTransition(int durationMillis) {
        from = 0;
        to = 255;
        alpha = 0;
        duration = originalDuration = durationMillis;
        reverse = false;
        transition = STARTING;
        invalidateSelf();
    }
    public void resetTransition() {
        alpha = 0;
        transition = IDLE;
        invalidateSelf();
    }
    public void reverseTransition(int duration) {
        long time = SystemClock.uptimeMillis();
        if (time - startTime > originalDuration) { /* finished: start again from the end it reached */
            if (to == 0) { from = 0; to = 255; alpha = 0; reverse = false; }
            else { from = 255; to = 0; alpha = 255; reverse = true; }
            this.duration = originalDuration = duration;
            transition = STARTING;
            invalidateSelf();
            return;
        }
        reverse = !reverse;
        from = alpha;
        to = reverse ? 0 : 255;
        this.duration = (int) (reverse ? time - startTime : originalDuration - (time - startTime));
        transition = STARTING;
    }
    public void setCrossFadeEnabled(boolean enabled) { crossFade = enabled; }
    public boolean isCrossFadeEnabled() { return crossFade; }

    @Override public void draw(Canvas canvas) {
        boolean done = true;
        if (transition == STARTING) {
            startTime = SystemClock.uptimeMillis();
            done = false;
            transition = RUNNING;
        } else if (transition == RUNNING && startTime >= 0) {
            float t = duration > 0 ? (SystemClock.uptimeMillis() - startTime) / (float) duration : 1;
            done = t >= 1;
            t = Math.min(t, 1);
            alpha = (int) (from + (to - from) * t);
        }
        Drawable d = getDrawable(0);
        if (done && alpha == 0) { d.draw(canvas); return; }
        if (crossFade) d.setAlpha(255 - alpha);
        d.draw(canvas);
        if (crossFade) d.setAlpha(0xff);
        if (alpha > 0) {
            d = getDrawable(1);
            d.setAlpha(alpha);
            d.draw(canvas);
            d.setAlpha(0xff);
        }
        if (!done) invalidateSelf();
    }
}
