// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

import java.text.BreakIterator;

/** Grapheme clusters as segments. */
public class GraphemeClusterSegmentFinder extends SegmentFinder {
    private final BreakIterator it = BreakIterator.getCharacterInstance();
    private final int length;

    public GraphemeClusterSegmentFinder(CharSequence text, TextPaint paint) {
        it.setText(text.toString());
        length = text.length();
    }

    @Override public int previousStartBoundary(int offset) { return offset <= 0 ? DONE : it.preceding(Math.min(offset, length)); }
    @Override public int previousEndBoundary(int offset) { return offset <= 0 ? DONE : it.preceding(Math.min(offset, length)) == 0 ? DONE : it.current(); }
    @Override public int nextStartBoundary(int offset) { int n = offset >= length ? DONE : it.following(offset); return n >= length ? DONE : n; }
    @Override public int nextEndBoundary(int offset) { return offset >= length ? DONE : it.following(offset); }
}
