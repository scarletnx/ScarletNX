// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database;

/** Told when a database is corrupt. */
public interface DatabaseErrorHandler {
    void onCorruption(android.database.sqlite.SQLiteDatabase dbObj);
}
