// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database.sqlite;

/** An SQLite error: SQLiteReadOnlyDatabaseException. */
public class SQLiteReadOnlyDatabaseException extends SQLiteException {
    public SQLiteReadOnlyDatabaseException() {}
    public SQLiteReadOnlyDatabaseException(String error) { super(error); }
    public SQLiteReadOnlyDatabaseException(String error, Throwable cause) { super(error, cause); }
}
