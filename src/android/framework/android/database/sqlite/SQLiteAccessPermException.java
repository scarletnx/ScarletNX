// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database.sqlite;

/** An SQLite error: SQLiteAccessPermException. */
public class SQLiteAccessPermException extends SQLiteException {
    public SQLiteAccessPermException() {}
    public SQLiteAccessPermException(String error) { super(error); }
    public SQLiteAccessPermException(String error, Throwable cause) { super(error, cause); }
}
