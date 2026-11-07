// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database.sqlite;

/** An SQLite error: SQLiteDatatypeMismatchException. */
public class SQLiteDatatypeMismatchException extends SQLiteException {
    public SQLiteDatatypeMismatchException() {}
    public SQLiteDatatypeMismatchException(String error) { super(error); }
    public SQLiteDatatypeMismatchException(String error, Throwable cause) { super(error, cause); }
}
