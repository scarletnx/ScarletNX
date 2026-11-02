// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database.sqlite;

/** An SQLite error: SQLiteAbortException. */
public class SQLiteAbortException extends SQLiteException {
    public SQLiteAbortException() {}
    public SQLiteAbortException(String error) { super(error); }
    public SQLiteAbortException(String error, Throwable cause) { super(error, cause); }
}
