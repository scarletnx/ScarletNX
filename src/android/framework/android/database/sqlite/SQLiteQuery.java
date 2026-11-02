// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database.sqlite;

/** A query's statement. */
public final class SQLiteQuery extends SQLiteProgram {
    SQLiteQuery(SQLiteDatabase db, String query, Object[] args) { super(db, query, args); }
    @Override public String toString() { return "SQLiteQuery: " + sql; }
}
