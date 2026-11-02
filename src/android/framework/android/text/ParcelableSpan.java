// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.text;

/** A span that can be written to a Parcel. */
public interface ParcelableSpan extends android.os.Parcelable {
    int getSpanTypeId();
}
