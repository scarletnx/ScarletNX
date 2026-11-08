// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.nfc;

/** A tag an NFC adapter found: none is found here, as the device has no adapter. */
public final class Tag implements android.os.Parcelable {
    private final byte[] id;
    private final String[] techList;

    Tag(byte[] id, String[] techList) {
        this.id = id;
        this.techList = techList;
    }
    public byte[] getId() { return id.clone(); }
    public String[] getTechList() { return techList.clone(); }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeValue(this); }
    public static final android.os.Parcelable.Creator<Tag> CREATOR = new android.os.Parcelable.Creator<>() {
        @Override public Tag createFromParcel(android.os.Parcel in) { return (Tag) in.readValue(null); }
        @Override public Tag[] newArray(int size) { return new Tag[size]; }
    };
}
