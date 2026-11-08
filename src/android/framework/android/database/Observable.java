// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2007 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.database;

import java.util.ArrayList;

/** A list of observers. Each observer can be registered once. */
public abstract class Observable<T> {
    protected final ArrayList<T> mObservers = new ArrayList<>();

    public Observable() {}

    public void registerObserver(T observer) {
        if (observer == null) throw new IllegalArgumentException("The observer is null.");
        synchronized (mObservers) {
            if (mObservers.contains(observer)) throw new IllegalStateException("Observer " + observer + " is already registered.");
            mObservers.add(observer);
        }
    }

    public void unregisterObserver(T observer) {
        if (observer == null) throw new IllegalArgumentException("The observer is null.");
        synchronized (mObservers) {
            int index = mObservers.indexOf(observer);
            if (index == -1) throw new IllegalStateException("Observer " + observer + " was not registered.");
            mObservers.remove(index);
        }
    }

    public void unregisterAll() {
        synchronized (mObservers) { mObservers.clear(); }
    }
}
