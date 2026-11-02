// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.style;

import android.graphics.Paint;
import android.text.TextPaint;

/** A span that sets the height of the lines it covers. */
public interface LineHeightSpan extends ParagraphStyle, WrapTogetherSpan {
    void chooseHeight(CharSequence text, int start, int end, int spanstartv, int lineHeight, Paint.FontMetricsInt fm);

    interface WithDensity extends LineHeightSpan {
        void chooseHeight(CharSequence text, int start, int end, int spanstartv, int lineHeight, Paint.FontMetricsInt fm, TextPaint paint);
    }

    class Standard implements LineHeightSpan {
        private final int height;

        public Standard(int height) { this.height = height; }
        public int getHeight() { return height; }
        @Override public void chooseHeight(CharSequence text, int start, int end, int spanstartv, int lineHeight, Paint.FontMetricsInt fm) {
            int original = fm.descent - fm.ascent;
            if (original <= 0) return;
            float ratio = height / (float) original;
            fm.descent = Math.round(fm.descent * ratio);
            fm.ascent = fm.descent - height;
        }
    }
}
