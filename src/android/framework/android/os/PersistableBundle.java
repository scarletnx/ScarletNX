// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** A Bundle of the values that can be written to storage. */
public final class PersistableBundle extends BaseBundle implements Cloneable, Parcelable {
    public static final PersistableBundle EMPTY = new PersistableBundle();

    public PersistableBundle() {}
    public PersistableBundle(int capacity) { super(capacity); }
    public PersistableBundle(PersistableBundle b) { super(b); }

    public void putPersistableBundle(String key, PersistableBundle value) { map.put(key, value); }
    public PersistableBundle getPersistableBundle(String key) { return typed(key, PersistableBundle.class); }
    @Override public Object clone() { return new PersistableBundle(this); }
    public PersistableBundle deepCopy() { return new PersistableBundle(this); }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }

    public static final Parcelable.Creator<PersistableBundle> CREATOR = new Parcelable.Creator<>() {
        @Override public PersistableBundle createFromParcel(Parcel in) { return (PersistableBundle) in.readValue(null); }
        @Override public PersistableBundle[] newArray(int size) { return new PersistableBundle[size]; }
    };
}
