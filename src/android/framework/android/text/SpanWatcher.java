// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

/** Told when spans of a text change. */
public interface SpanWatcher extends NoCopySpan {
    void onSpanAdded(Spannable text, Object what, int start, int end);
    void onSpanRemoved(Spannable text, Object what, int start, int end);
    void onSpanChanged(Spannable text, Object what, int ostart, int oend, int nstart, int nend);
}
