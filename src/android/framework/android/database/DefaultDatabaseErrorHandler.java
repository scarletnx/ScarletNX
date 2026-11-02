// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database;

/** Deletes a corrupt database, as Android's does. */
public final class DefaultDatabaseErrorHandler implements DatabaseErrorHandler {
    @Override public void onCorruption(android.database.sqlite.SQLiteDatabase dbObj) {
        String path = dbObj.getPath();
        if (path != null && !path.equals(":memory:")) android.database.sqlite.SQLiteDatabase.deleteDatabase(new java.io.File(path));
    }
}
