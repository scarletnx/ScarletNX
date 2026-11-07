// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.view;

import android.os.Parcel;
import android.os.Parcelable;

/** A view's saved state, with its superclass's inside. */
public abstract class AbsSavedState implements Parcelable {
    public static final AbsSavedState EMPTY_STATE = new AbsSavedState() {};
    private final Parcelable superState;

    private AbsSavedState() { superState = null; }
    protected AbsSavedState(Parcelable superState) {
        if (superState == null) throw new IllegalArgumentException("superState must not be null");
        this.superState = superState != EMPTY_STATE ? superState : null;
    }
    protected AbsSavedState(Parcel source) { this(source, null); }
    protected AbsSavedState(Parcel source, ClassLoader loader) {
        Parcelable s = source.readParcelable(loader);
        superState = s != null ? s : EMPTY_STATE;
    }
    public final Parcelable getSuperState() { return superState; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeParcelable(superState, flags); }
}
