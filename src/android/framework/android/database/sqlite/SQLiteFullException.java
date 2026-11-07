// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database.sqlite;

/** An SQLite error: SQLiteFullException. */
public class SQLiteFullException extends SQLiteException {
    public SQLiteFullException() {}
    public SQLiteFullException(String error) { super(error); }
    public SQLiteFullException(String error, Throwable cause) { super(error, cause); }
}
