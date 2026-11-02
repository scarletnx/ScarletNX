// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text.style;

import android.graphics.Canvas;
import android.graphics.Paint;
import android.text.Layout;

/** A margin before a paragraph's lines. */
public interface LeadingMarginSpan extends ParagraphStyle {
    int getLeadingMargin(boolean first);

    void drawLeadingMargin(Canvas c, Paint p, int x, int dir, int top, int baseline, int bottom, CharSequence text,
                           int start, int end, boolean first, Layout layout);

    interface LeadingMarginSpan2 extends LeadingMarginSpan, WrapTogetherSpan {
        int getLeadingMarginLineCount();
    }

    class Standard implements LeadingMarginSpan {
        private final int first, rest;

        public Standard(int first, int rest) { this.first = first; this.rest = rest; }
        public Standard(int every) { this(every, every); }
        @Override public int getLeadingMargin(boolean first) { return first ? this.first : rest; }
        @Override public void drawLeadingMargin(Canvas c, Paint p, int x, int dir, int top, int baseline, int bottom,
                                                CharSequence text, int start, int end, boolean first, Layout layout) {}
    }
}
