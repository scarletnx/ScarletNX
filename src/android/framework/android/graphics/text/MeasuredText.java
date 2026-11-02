// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.graphics.text;

import android.graphics.Paint;
import android.graphics.Rect;

/** Text measured with its styles, runs of a paint each. */
public class MeasuredText {
    private final char[] text;
    private final float[] advances;

    MeasuredText(char[] text, float[] advances) {
        this.text = text;
        this.advances = advances;
    }

    /** The text, for drawing it. */
    public char[] text() { return text; }

    public float getWidth(int start, int end) {
        float w = 0;
        for (int i = start; i < end; ++i) w += advances[i];
        return w;
    }

    public float getCharWidthAt(int offset) { return advances[offset]; }
    public void getBounds(int start, int end, Rect rect) { rect.set(0, 0, (int) Math.ceil(getWidth(start, end)), 0); }
    public void getFontMetricsInt(int start, int end, Paint.FontMetricsInt outMetrics) {}

    public static final class Builder {
        private final char[] text;
        private final float[] advances;

        public Builder(char[] text) {
            this.text = text;
            advances = new float[text.length];
        }

        public Builder(MeasuredText text) {
            this.text = text.text;
            advances = text.advances.clone();
        }

        public Builder appendStyleRun(Paint paint, int length, boolean isRtl) { return appendStyleRun(paint, null, length, isRtl); }

        private int at;

        public Builder appendStyleRun(Paint paint, LineBreakConfig config, int length, boolean isRtl) {
            paint.getTextRunAdvances(text, at, length, at, length, isRtl, advances, at);
            at += length;
            return this;
        }

        public Builder appendReplacementRun(Paint paint, int length, float width) {
            if (length > 0) advances[at] = width;
            at += length;
            return this;
        }

        public Builder setComputeHyphenation(boolean computeHyphenation) { return this; }
        public Builder setComputeHyphenation(int mode) { return this; }
        public Builder setComputeLayout(boolean computeLayout) { return this; }

        public MeasuredText build() { return new MeasuredText(text, advances); }
    }
}
