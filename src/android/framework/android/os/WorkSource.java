// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

import java.util.Arrays;

/** The uids that work is done for, as the battery statistics count it. */
public class WorkSource implements Parcelable {
    private int[] uids = new int[0];

    public WorkSource() {}
    public WorkSource(WorkSource orig) { if (orig != null) uids = orig.uids.clone(); }

    public int size() { return uids.length; }
    public void clear() { uids = new int[0]; }
    public boolean isEmpty() { return uids.length == 0; }

    public boolean add(WorkSource other) {
        boolean changed = false;
        for (int u : other.uids) changed |= addUid(u);
        return changed;
    }

    private boolean addUid(int uid) {
        if (Arrays.binarySearch(uids, uid) >= 0) return false;
        uids = Arrays.copyOf(uids, uids.length + 1);
        uids[uids.length - 1] = uid;
        Arrays.sort(uids);
        return true;
    }

    public boolean remove(WorkSource other) {
        int[] kept = Arrays.stream(uids).filter(u -> Arrays.binarySearch(other.uids, u) < 0).toArray();
        boolean changed = kept.length != uids.length;
        uids = kept;
        return changed;
    }

    public void set(WorkSource other) { uids = other != null ? other.uids.clone() : new int[0]; }
    public boolean diff(WorkSource other) { return !Arrays.equals(uids, other.uids); }

    @Override public boolean equals(Object o) { return o instanceof WorkSource w && Arrays.equals(uids, w.uids); }
    @Override public int hashCode() { return Arrays.hashCode(uids); }
    @Override public String toString() { return "WorkSource{" + Arrays.toString(uids) + "}"; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeIntArray(uids); }
    public static final Parcelable.Creator<WorkSource> CREATOR = new Parcelable.Creator<>() {
        @Override public WorkSource createFromParcel(Parcel in) {
            WorkSource w = new WorkSource();
            int[] u = in.createIntArray();
            if (u != null) w.uids = u;
            return w;
        }
        @Override public WorkSource[] newArray(int size) { return new WorkSource[size]; }
    };
}
