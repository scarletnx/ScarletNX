// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database.sqlite;

/** An SQLite error: SQLiteMisuseException. */
public class SQLiteMisuseException extends SQLiteException {
    public SQLiteMisuseException() {}
    public SQLiteMisuseException(String error) { super(error); }
    public SQLiteMisuseException(String error, Throwable cause) { super(error, cause); }
}
