// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database.sqlite;

import android.database.AbstractCursor;
import android.database.CursorIndexOutOfBoundsException;
import java.util.ArrayList;
import snx.android.db.SQLite;

/** A query's rows: read whole when the cursor is first used, as a CursorWindow holds them. */
public class SQLiteCursor extends AbstractCursor {
    private final SQLiteQuery query;
    private final SQLiteCursorDriver driver;
    private final String[] columns;
    private ArrayList<Object[]> rows;

    public SQLiteCursor(SQLiteCursorDriver driver, String editTable, SQLiteQuery query) {
        this.driver = driver;
        this.query = query;
        columns = query.getColumnNames();
    }
    @Deprecated public SQLiteCursor(SQLiteDatabase db, SQLiteCursorDriver driver, String editTable, SQLiteQuery query) { this(driver, editTable, query); }

    private ArrayList<Object[]> rows() {
        if (rows == null) {
            rows = query.database.withStatement(query.sql, query.args, s -> {
                ArrayList<Object[]> out = new ArrayList<>();
                int n = SQLite.columnCount(s);
                while (SQLite.step(s) == SQLite.ROW) {
                    Object[] row = new Object[n];
                    for (int i = 0; i < n; ++i) {
                        switch (SQLite.columnType(s, i)) {
                            case SQLite.INTEGER -> row[i] = SQLite.columnLong(s, i);
                            case SQLite.FLOAT -> row[i] = SQLite.columnDouble(s, i);
                            case SQLite.TEXT -> row[i] = SQLite.columnString(s, i);
                            case SQLite.BLOB -> row[i] = SQLite.columnBlob(s, i);
                            default -> row[i] = null;
                        }
                    }
                    out.add(row);
                }
                return out;
            });
        }
        return rows;
    }
    private Object get(int column) {
        if (column < 0 || column >= columns.length) throw new IllegalStateException("Couldn't read row " + mPos + ", col " + column + " from CursorWindow.");
        if (mPos < 0 || mPos >= rows().size()) throw new CursorIndexOutOfBoundsException(mPos, rows().size());
        return rows().get(mPos)[column];
    }
    public SQLiteDatabase getDatabase() { return query.database; }
    @Override public int getCount() { return rows().size(); }
    @Override public String[] getColumnNames() { return columns; }
    @Override public String getString(int column) {
        Object v = get(column);
        if (v instanceof byte[] b) return new String(b, java.nio.charset.StandardCharsets.UTF_8);
        if (v instanceof Double d && d == Math.rint(d) && !Double.isInfinite(d)) return Double.toString(d);
        return v != null ? v.toString() : null;
    }
    private static double num(Object v) {
        if (v == null) return 0;
        if (v instanceof Number n) return n.doubleValue();
        try { return Double.parseDouble(v.toString()); } catch (NumberFormatException e) { return 0; }
    }
    @Override public short getShort(int column) { return (short) getLong(column); }
    @Override public int getInt(int column) { return (int) getLong(column); }
    @Override public long getLong(int column) { Object v = get(column); return v instanceof Long l ? l : (long) num(v); }
    @Override public float getFloat(int column) { return (float) num(get(column)); }
    @Override public double getDouble(int column) { return num(get(column)); }
    @Override public byte[] getBlob(int column) {
        Object v = get(column);
        return v instanceof byte[] b ? b : v != null ? v.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8) : null;
    }
    @Override public int getType(int column) {
        Object v = get(column);
        if (v == null) return FIELD_TYPE_NULL;
        if (v instanceof Long) return FIELD_TYPE_INTEGER;
        if (v instanceof Double) return FIELD_TYPE_FLOAT;
        if (v instanceof byte[]) return FIELD_TYPE_BLOB;
        return FIELD_TYPE_STRING;
    }
    @Override public boolean isNull(int column) { return get(column) == null; }
    @Override public boolean requery() { rows = null; return true; }
    @Override public void close() { super.close(); rows = null; if (driver != null) driver.cursorClosed(); }
    public void setSelectionArguments(String[] selectionArgs) { query.bindAllArgsAsStrings(selectionArgs); rows = null; }
    public void setWindow(Object window) {}
    public void setFillWindowForwardOnly(boolean fillWindowForwardOnly) {}
}
