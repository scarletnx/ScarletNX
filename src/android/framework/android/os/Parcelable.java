// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

/** An object that writes itself to a Parcel. */
public interface Parcelable {
    int PARCELABLE_WRITE_RETURN_VALUE = 1;
    int CONTENTS_FILE_DESCRIPTOR = 1;

    int describeContents();

    void writeToParcel(Parcel dest, int flags);

    interface Creator<T> {
        T createFromParcel(Parcel source);

        T[] newArray(int size);
    }

    interface ClassLoaderCreator<T> extends Creator<T> {
        T createFromParcel(Parcel source, ClassLoader loader);
    }
}
