// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app.slice;

/** The type and revision of a slice's format (deprecated). */
@Deprecated
public final class SliceSpec implements android.os.Parcelable {
    private final String type;
    private final int revision;

    public SliceSpec(String type, int revision) { this.type = type; this.revision = revision; }
    public String getType() { return type; }
    public int getRevision() { return revision; }
    /** True when the candidate has the same type and a revision no newer than this one. */
    public boolean canRender(SliceSpec candidate) { return type.equals(candidate.type) && revision >= candidate.revision; }
    @Override public boolean equals(Object o) { return o instanceof SliceSpec s && type.equals(s.type) && revision == s.revision; }
    @Override public int hashCode() { return type.hashCode() * 31 + revision; }
    @Override public String toString() { return "SliceSpec{" + type + "," + revision + "}"; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeValue(this); }
}
