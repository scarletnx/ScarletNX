// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

/** Segments of a text (graphemes, words) by their boundaries. */
public abstract class SegmentFinder {
    public static final int DONE = -1;

    public abstract int previousStartBoundary(int offset);
    public abstract int previousEndBoundary(int offset);
    public abstract int nextStartBoundary(int offset);
    public abstract int nextEndBoundary(int offset);

    public static class PrescribedSegmentFinder extends SegmentFinder {
        private final int[] segments;

        public PrescribedSegmentFinder(int[] segments) { this.segments = segments.clone(); }
        @Override public int previousStartBoundary(int offset) { for (int i = segments.length - 2; i >= 0; i -= 2) if (segments[i] < offset) return segments[i]; return DONE; }
        @Override public int previousEndBoundary(int offset) { for (int i = segments.length - 1; i >= 1; i -= 2) if (segments[i] < offset) return segments[i]; return DONE; }
        @Override public int nextStartBoundary(int offset) { for (int i = 0; i < segments.length; i += 2) if (segments[i] > offset) return segments[i]; return DONE; }
        @Override public int nextEndBoundary(int offset) { for (int i = 1; i < segments.length; i += 2) if (segments[i] > offset) return segments[i]; return DONE; }
    }
}
