// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2007 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.database.sqlite;

import java.io.Closeable;

/** An object of a database that is closed when its last reference goes. */
public abstract class SQLiteClosable implements Closeable {
    private int references = 1;

    protected abstract void onAllReferencesReleased();
    public void acquireReference() {
        synchronized (this) {
            if (references <= 0) throw new IllegalStateException("attempt to re-open an already-closed object: " + this);
            ++references;
        }
    }
    public void releaseReference() {
        boolean last;
        synchronized (this) { last = --references == 0; }
        if (last) onAllReferencesReleased();
    }
    @Deprecated public void releaseReferenceFromContainer() { releaseReference(); }
    @Override public void close() { releaseReference(); }
}
