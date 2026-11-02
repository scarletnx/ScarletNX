// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

/** A key and value marking a range of text. */
public class Annotation implements ParcelableSpan {
    private final String key, value;

    public Annotation(String key, String value) { this.key = key; this.value = value; }
    public String getKey() { return key; }
    public String getValue() { return value; }
    @Override public int getSpanTypeId() { return 18; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int flags) { dest.writeString(key); dest.writeString(value); }
}
