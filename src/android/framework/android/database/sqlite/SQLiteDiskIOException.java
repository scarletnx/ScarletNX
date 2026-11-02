// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database.sqlite;

/** An SQLite error: SQLiteDiskIOException. */
public class SQLiteDiskIOException extends SQLiteException {
    public SQLiteDiskIOException() {}
    public SQLiteDiskIOException(String error) { super(error); }
    public SQLiteDiskIOException(String error, Throwable cause) { super(error, cause); }
}
