// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database.sqlite;

import snx.android.db.SQLite;

/** A statement run again and again with its arguments. */
public final class SQLiteStatement extends SQLiteProgram {
    SQLiteStatement(SQLiteDatabase db, String sql, Object[] bindArgs) { super(db, sql, bindArgs); }

    public void execute() { database.run(sql, args, s -> null); }
    public int executeUpdateDelete() { return database.run(sql, args, s -> SQLite.changes(database.handle())); }
    public long executeInsert() {
        return database.run(sql, args, s -> SQLite.changes(database.handle()) > 0 ? SQLite.lastInsertRowId(database.handle()) : -1L);
    }
    public long simpleQueryForLong() {
        return database.withStatement(sql, args, s -> {
            if (SQLite.step(s) != SQLite.ROW) throw new SQLiteDoneException();
            return SQLite.columnLong(s, 0);
        });
    }
    public String simpleQueryForString() {
        return database.withStatement(sql, args, s -> {
            if (SQLite.step(s) != SQLite.ROW) throw new SQLiteDoneException();
            return SQLite.columnString(s, 0);
        });
    }
    public android.os.ParcelFileDescriptor simpleQueryForBlobFileDescriptor() { throw new UnsupportedOperationException("snx: blob file descriptors"); }
    @Override public String toString() { return "SQLiteProgram: " + sql; }
}
