// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.print;

import android.os.Parcel;
import android.os.Parcelable;

/** A range of pages, from start to end, both included. */
public final class PageRange implements Parcelable {
    public static final PageRange ALL_PAGES = new PageRange(0, Integer.MAX_VALUE);
    private final int start, end;

    public PageRange(int start, int end) {
        if (start < 0) throw new IllegalArgumentException("start cannot be less than zero.");
        if (end < 0) throw new IllegalArgumentException("end cannot be less than zero.");
        if (start > end) throw new IllegalArgumentException("start must be lesser than end.");
        this.start = start; this.end = end;
    }
    public int getStart() { return start; }
    public int getEnd() { return end; }
    @Override public boolean equals(Object o) { return o instanceof PageRange r && r.start == start && r.end == end; }
    @Override public int hashCode() { return start * 31 + end; }
    @Override public String toString() { return this == ALL_PAGES ? "PageRange[<all pages>]" : "PageRange[" + start + " - " + end + "]"; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<PageRange> CREATOR = new Parcelable.Creator<>() {
        @Override public PageRange createFromParcel(Parcel in) { return (PageRange) in.readValue(null); }
        @Override public PageRange[] newArray(int size) { return new PageRange[size]; }
    };
}
