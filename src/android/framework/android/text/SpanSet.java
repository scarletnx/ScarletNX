// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

import java.lang.reflect.Array;
import java.util.ArrayList;

/** The spans of a text: each with its range and flags, in the order they were set. */
final class SpanSet {
    final ArrayList<Object> spans = new ArrayList<>();
    final ArrayList<int[]> ranges = new ArrayList<>(); // start, end, flags

    SpanSet() {}

    SpanSet(SpanSet o) {
        spans.addAll(o.spans);
        for (int[] r : o.ranges) ranges.add(r.clone());
    }

    void set(Object what, int start, int end, int flags, int length) {
        if (start > end || start < 0 || end > length)
            throw new IndexOutOfBoundsException("setSpan (" + start + " ... " + end + ") ends beyond length " + length);
        int i = spans.indexOf(what);
        if (i >= 0) {
            ranges.set(i, new int[] {start, end, flags});
            return;
        }
        // a higher priority comes first, as Android orders them
        int priority = flags & Spanned.SPAN_PRIORITY;
        int at = spans.size();
        if (priority != 0) {
            for (at = 0; at < spans.size(); ++at) if ((ranges.get(at)[2] & Spanned.SPAN_PRIORITY) < priority) break;
        }
        spans.add(at, what);
        ranges.add(at, new int[] {start, end, flags});
    }

    void remove(Object what) {
        int i = spans.indexOf(what);
        if (i >= 0) { spans.remove(i); ranges.remove(i); }
    }

    int start(Object what) { int i = spans.indexOf(what); return i >= 0 ? ranges.get(i)[0] : -1; }
    int end(Object what) { int i = spans.indexOf(what); return i >= 0 ? ranges.get(i)[1] : -1; }
    int flags(Object what) { int i = spans.indexOf(what); return i >= 0 ? ranges.get(i)[2] : 0; }

    @SuppressWarnings("unchecked")
    <T> T[] get(int start, int end, Class<T> type) {
        ArrayList<Object> out = new ArrayList<>();
        Class<?> k = type != null ? type : Object.class;
        for (int i = 0; i < spans.size(); ++i) {
            Object s = spans.get(i);
            if (!k.isInstance(s)) continue;
            int[] r = ranges.get(i);
            int ss = r[0], se = r[1];
            if (ss > end || se < start) continue;
            if (ss != se && start != end) {
                if (ss == end || se == start) continue;
            }
            out.add(s);
        }
        return out.toArray((T[]) Array.newInstance(k, out.size()));
    }

    int nextTransition(int start, int limit, Class<?> type) {
        Class<?> k = type != null ? type : Object.class;
        for (int i = 0; i < spans.size(); ++i) {
            if (!k.isInstance(spans.get(i))) continue;
            int[] r = ranges.get(i);
            if (r[0] > start && r[0] < limit) limit = r[0];
            if (r[1] > start && r[1] < limit) limit = r[1];
        }
        return limit;
    }

    /** The ranges moved for text [start, end) replaced by count chars, by the spans' point and mark flags. */
    void replace(int start, int end, int count) {
        int delta = count - (end - start);
        for (int i = spans.size() - 1; i >= 0; --i) {
            int[] r = ranges.get(i);
            r[0] = move(r[0], start, end, delta, (r[2] & 0xf0) == 0x20);
            r[1] = move(r[1], start, end, delta, (r[2] & 0x0f) == 0x02);
            if (r[0] > r[1]) r[0] = r[1];
            if (r[0] == r[1] && (r[2] & Spanned.SPAN_POINT_MARK_MASK) == Spanned.SPAN_EXCLUSIVE_EXCLUSIVE && end > start && count == 0
                && r[0] == start) {
                spans.remove(i);
                ranges.remove(i);
            }
        }
    }

    /** A span boundary at at, for a replacement of [start, end): a point moves to the end of what goes in. */
    private static int move(int at, int start, int end, int delta, boolean point) {
        if (at < start) return at;
        if (at > end) return at + delta;
        if (at == start && start == end) return point ? at + delta : at;
        if (at == end) return at + delta;
        return point ? end + delta : start;
    }
}
