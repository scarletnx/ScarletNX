// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2007 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.database;

/** Tells its DataSetObservers of a change, the last registered first. */
public class DataSetObservable extends Observable<DataSetObserver> {
    public DataSetObservable() {}

    public void notifyChanged() {
        synchronized (mObservers) {
            for (int i = mObservers.size() - 1; i >= 0; --i) mObservers.get(i).onChanged();
        }
    }

    public void notifyInvalidated() {
        synchronized (mObservers) {
            for (int i = mObservers.size() - 1; i >= 0; --i) mObservers.get(i).onInvalidated();
        }
    }
}
