// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database;

import java.util.ArrayList;

/** Rows kept in memory: a query's result, or rows an app builds. */
public class MatrixCursor extends AbstractCursor {
    private final String[] columns;
    private final ArrayList<Object[]> rows = new ArrayList<>();

    public MatrixCursor(String[] columnNames) { this(columnNames, 16); }
    public MatrixCursor(String[] columnNames, int initialCapacity) { columns = columnNames; }

    public class RowBuilder {
        private final Object[] row;
        private int index;
        RowBuilder(Object[] row) { this.row = row; }
        public RowBuilder add(Object columnValue) { row[index++] = columnValue; return this; }
        public RowBuilder add(String columnName, Object value) {
            for (int i = 0; i < columns.length; ++i) if (columns[i].equals(columnName)) row[i] = value;
            return this;
        }
    }

    public RowBuilder newRow() { Object[] r = new Object[columns.length]; rows.add(r); return new RowBuilder(r); }
    public void addRow(Object[] columnValues) {
        if (columnValues.length != columns.length) throw new IllegalArgumentException("columnNames.length = " + columns.length + ", columnValues.length = " + columnValues.length);
        rows.add(columnValues.clone());
    }
    public void addRow(Iterable<?> columnValues) {
        ArrayList<Object> v = new ArrayList<>();
        for (Object o : columnValues) v.add(o);
        addRow(v.toArray());
    }
    private Object get(int column) {
        if (column < 0 || column >= columns.length) throw new CursorIndexOutOfBoundsException("Requested column: " + column + ", # of columns: " + columns.length);
        checkPosition();
        return rows.get(mPos)[column];
    }
    @Override public int getCount() { return rows.size(); }
    @Override public String[] getColumnNames() { return columns; }
    @Override public String getString(int column) { Object v = get(column); return v != null ? v.toString() : null; }
    @Override public short getShort(int column) { Object v = get(column); return v == null ? 0 : v instanceof Number n ? n.shortValue() : Short.parseShort(v.toString()); }
    @Override public int getInt(int column) { Object v = get(column); return v == null ? 0 : v instanceof Number n ? n.intValue() : Integer.parseInt(v.toString()); }
    @Override public long getLong(int column) { Object v = get(column); return v == null ? 0 : v instanceof Number n ? n.longValue() : Long.parseLong(v.toString()); }
    @Override public float getFloat(int column) { Object v = get(column); return v == null ? 0 : v instanceof Number n ? n.floatValue() : Float.parseFloat(v.toString()); }
    @Override public double getDouble(int column) { Object v = get(column); return v == null ? 0 : v instanceof Number n ? n.doubleValue() : Double.parseDouble(v.toString()); }
    @Override public byte[] getBlob(int column) { Object v = get(column); return v instanceof byte[] b ? b : null; }
    @Override public int getType(int column) {
        Object v = get(column);
        if (v == null) return FIELD_TYPE_NULL;
        if (v instanceof byte[]) return FIELD_TYPE_BLOB;
        if (v instanceof Float || v instanceof Double) return FIELD_TYPE_FLOAT;
        if (v instanceof Number) return FIELD_TYPE_INTEGER;
        return FIELD_TYPE_STRING;
    }
    @Override public boolean isNull(int column) { return get(column) == null; }
}
