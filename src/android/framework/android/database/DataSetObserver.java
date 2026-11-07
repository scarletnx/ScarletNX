// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database;

/** Told when a data set changes. */
public abstract class DataSetObserver {
    public void onChanged() {}
    public void onInvalidated() {}
}
