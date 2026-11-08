// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database.sqlite;

/** An SQLite error: SQLiteTableLockedException. */
public class SQLiteTableLockedException extends SQLiteException {
    public SQLiteTableLockedException() {}
    public SQLiteTableLockedException(String error) { super(error); }
    public SQLiteTableLockedException(String error, Throwable cause) { super(error, cause); }
}
