// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database.sqlite;

import snx.android.db.SQLite;

/** A compiled statement and its arguments. */
public abstract class SQLiteProgram extends SQLiteClosable {
    final SQLiteDatabase database;
    final String sql;
    final Object[] args;
    final boolean readOnly;
    private final String[] columns;

    SQLiteProgram(SQLiteDatabase db, String sql, Object[] bindArgs) {
        database = db;
        this.sql = sql.trim();
        int[] count = new int[1];
        boolean[] ro = new boolean[1];
        String[][] names = new String[1][];
        db.withStatement(this.sql, null, statement -> {
            count[0] = SQLite.parameterCount(statement);
            ro[0] = SQLite.readOnly(statement);
            int n = SQLite.columnCount(statement);
            names[0] = new String[n];
            for (int i = 0; i < n; ++i) names[0][i] = SQLite.columnName(statement, i);
            return null;
        });
        args = new Object[count[0]];
        readOnly = ro[0];
        columns = names[0];
        if (bindArgs != null) {
            if (bindArgs.length > args.length) throw new IllegalArgumentException("Too many bind arguments.  " + bindArgs.length + " arguments were provided but the statement needs " + args.length + " arguments.");
            System.arraycopy(bindArgs, 0, args, 0, bindArgs.length);
        }
    }

    final SQLiteDatabase getDatabase() { return database; }
    final String getSql() { return sql; }
    final String[] getColumnNames() { return columns; }
    public final int getUniqueId() { return -1; }
    private void bind(int index, Object value) {
        if (index < 1 || index > args.length) throw new IllegalArgumentException("Cannot bind argument at index " + index + " because the index is out of range.  The statement has " + args.length + " parameters.");
        args[index - 1] = value;
    }
    public void bindNull(int index) { bind(index, null); }
    public void bindLong(int index, long value) { bind(index, value); }
    public void bindDouble(int index, double value) { bind(index, value); }
    public void bindString(int index, String value) {
        if (value == null) throw new IllegalArgumentException("the bind value at index " + index + " is null");
        bind(index, value);
    }
    public void bindBlob(int index, byte[] value) {
        if (value == null) throw new IllegalArgumentException("the bind value at index " + index + " is null");
        bind(index, value);
    }
    public void clearBindings() { java.util.Arrays.fill(args, null); }
    public void bindAllArgsAsStrings(String[] bindArgs) {
        if (bindArgs != null) for (int i = bindArgs.length; i != 0; --i) bindString(i, bindArgs[i - 1]);
    }
    @Override protected void onAllReferencesReleased() { clearBindings(); }
}
