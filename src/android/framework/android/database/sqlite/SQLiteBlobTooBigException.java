// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database.sqlite;

/** An SQLite error: SQLiteBlobTooBigException. */
public class SQLiteBlobTooBigException extends SQLiteException {
    public SQLiteBlobTooBigException() {}
    public SQLiteBlobTooBigException(String error) { super(error); }
    public SQLiteBlobTooBigException(String error, Throwable cause) { super(error, cause); }
}
