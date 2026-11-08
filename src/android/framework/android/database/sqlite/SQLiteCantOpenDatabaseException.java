// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database.sqlite;

/** An SQLite error: SQLiteCantOpenDatabaseException. */
public class SQLiteCantOpenDatabaseException extends SQLiteException {
    public SQLiteCantOpenDatabaseException() {}
    public SQLiteCantOpenDatabaseException(String error) { super(error); }
    public SQLiteCantOpenDatabaseException(String error, Throwable cause) { super(error, cause); }
}
