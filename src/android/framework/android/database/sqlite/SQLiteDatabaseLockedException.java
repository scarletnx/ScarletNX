// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database.sqlite;

/** An SQLite error: SQLiteDatabaseLockedException. */
public class SQLiteDatabaseLockedException extends SQLiteException {
    public SQLiteDatabaseLockedException() {}
    public SQLiteDatabaseLockedException(String error) { super(error); }
    public SQLiteDatabaseLockedException(String error, Throwable cause) { super(error, cause); }
}
