// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database.sqlite;

/** An SQLite error: SQLiteConstraintException. */
public class SQLiteConstraintException extends SQLiteException {
    public SQLiteConstraintException() {}
    public SQLiteConstraintException(String error) { super(error); }
    public SQLiteConstraintException(String error, Throwable cause) { super(error, cause); }
}
