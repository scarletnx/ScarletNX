// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database.sqlite;

/** An SQLite error: SQLiteOutOfMemoryException. */
public class SQLiteOutOfMemoryException extends SQLiteException {
    public SQLiteOutOfMemoryException() {}
    public SQLiteOutOfMemoryException(String error) { super(error); }
    public SQLiteOutOfMemoryException(String error, Throwable cause) { super(error, cause); }
}
