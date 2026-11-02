// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database.sqlite;

/** An SQLite error: SQLiteDoneException. */
public class SQLiteDoneException extends SQLiteException {
    public SQLiteDoneException() {}
    public SQLiteDoneException(String error) { super(error); }
    public SQLiteDoneException(String error, Throwable cause) { super(error, cause); }
}
