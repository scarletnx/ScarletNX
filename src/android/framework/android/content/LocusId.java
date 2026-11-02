// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content;

/** An identifier of a piece of the app's state (a chat, a document), for shortcuts and notifications. */
public final class LocusId implements android.os.Parcelable {
    private final String id;

    public LocusId(String id) {
        if (id == null || id.isEmpty()) throw new IllegalArgumentException("id cannot be empty");
        this.id = id;
    }
    public String getId() { return id; }
    @Override public boolean equals(Object o) { return o instanceof LocusId l && l.id.equals(id); }
    @Override public int hashCode() { return id.hashCode(); }
    @Override public String toString() { return "LocusId[" + id.length() + "_chars]"; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeString(id); }
}
