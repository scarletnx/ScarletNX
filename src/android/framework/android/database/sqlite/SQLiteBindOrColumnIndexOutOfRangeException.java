// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database.sqlite;

/** An SQLite error: SQLiteBindOrColumnIndexOutOfRangeException. */
public class SQLiteBindOrColumnIndexOutOfRangeException extends SQLiteException {
    public SQLiteBindOrColumnIndexOutOfRangeException() {}
    public SQLiteBindOrColumnIndexOutOfRangeException(String error) { super(error); }
    public SQLiteBindOrColumnIndexOutOfRangeException(String error, Throwable cause) { super(error, cause); }
}
