// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.database;

import android.database.sqlite.SQLiteClosable;
import android.database.sqlite.SQLiteException;
import android.os.Parcel;
import android.os.Parcelable;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;

/**
 * A block of cursor rows, held in Java: each row an array of its values. The size limit counts the bytes that
 * the values take, as Android's shared memory window does.
 */
public class CursorWindow extends SQLiteClosable implements Parcelable {
    private static final long DEFAULT_SIZE = 2048 * 1024;

    private final String name;
    private final long capacity;
    private long used;
    private int startPos, numColumns;
    private final ArrayList<Object[]> rows = new ArrayList<>();

    public CursorWindow(String name) { this(name, DEFAULT_SIZE); }

    public CursorWindow(String name, long windowSizeBytes) {
        if (windowSizeBytes < 0) throw new IllegalArgumentException("Window size cannot be less than 0");
        this.name = name != null && !name.isEmpty() ? name : "<unnamed>";
        capacity = windowSizeBytes;
    }

    @Deprecated public CursorWindow(boolean localWindow) { this((String) null); }

    public String getName() { return name; }
    public int getStartPosition() { return startPos; }
    public void setStartPosition(int pos) { startPos = pos; }
    public int getNumRows() { return rows.size(); }

    public void clear() {
        startPos = 0;
        numColumns = 0;
        used = 0;
        rows.clear();
    }

    public boolean setNumColumns(int columnNum) {
        if (!rows.isEmpty() && columnNum != numColumns) return false;
        numColumns = columnNum;
        return true;
    }

    public boolean allocRow() {
        long size = 8L * numColumns;
        if (used + size > capacity) return false;
        used += size;
        rows.add(new Object[numColumns]);
        return true;
    }

    public void freeLastRow() {
        if (rows.isEmpty()) return;
        Object[] row = rows.remove(rows.size() - 1);
        used -= 8L * numColumns;
        for (Object v : row) used -= sizeOf(v);
    }

    private static long sizeOf(Object v) {
        if (v instanceof byte[] b) return b.length;
        if (v instanceof String s) return 2L * s.length() + 1;
        return 0;
    }

    private Object[] row(int row) {
        int r = row - startPos;
        if (r < 0 || r >= rows.size()) throw new IllegalStateException("Couldn't read row " + row + " from CursorWindow.");
        return rows.get(r);
    }

    private Object value(int row, int column) {
        Object[] r = row(row);
        if (column < 0 || column >= r.length) throw new IllegalStateException("Couldn't read row " + row + ", col " + column + " from CursorWindow.");
        return r[column];
    }

    private boolean put(Object v, int row, int column) {
        int r = row - startPos;
        if (r < 0 || r >= rows.size() || column < 0 || column >= numColumns) return false;
        Object[] values = rows.get(r);
        long grow = sizeOf(v) - sizeOf(values[column]);
        if (used + grow > capacity) return false;
        used += grow;
        values[column] = v;
        return true;
    }

    public int getType(int row, int column) {
        Object v = value(row, column);
        if (v == null) return Cursor.FIELD_TYPE_NULL;
        if (v instanceof Long) return Cursor.FIELD_TYPE_INTEGER;
        if (v instanceof Double) return Cursor.FIELD_TYPE_FLOAT;
        if (v instanceof String) return Cursor.FIELD_TYPE_STRING;
        return Cursor.FIELD_TYPE_BLOB;
    }

    public boolean isNull(int row, int column) { return value(row, column) == null; }

    public byte[] getBlob(int row, int column) {
        Object v = value(row, column);
        if (v == null || v instanceof byte[]) return (byte[]) v;
        if (v instanceof String s) return s.getBytes(StandardCharsets.UTF_8);
        throw new SQLiteException("INTEGER data in getBlob ");
    }

    public String getString(int row, int column) {
        Object v = value(row, column);
        if (v == null || v instanceof String) return (String) v;
        if (v instanceof Long || v instanceof Double) return v.toString();
        throw new SQLiteException("Unable to convert BLOB to string");
    }

    public void copyStringToBuffer(int row, int column, CharArrayBuffer buffer) {
        if (buffer == null) throw new IllegalArgumentException("CharArrayBuffer should not be null");
        String s = getString(row, column);
        char[] c = s != null ? s.toCharArray() : new char[0];
        if (buffer.data == null || buffer.data.length < c.length) buffer.data = c; else System.arraycopy(c, 0, buffer.data, 0, c.length);
        buffer.sizeCopied = c.length;
    }

    public long getLong(int row, int column) {
        Object v = value(row, column);
        if (v == null) return 0;
        if (v instanceof Long l) return l;
        if (v instanceof Double d) return d.longValue();
        if (v instanceof String s) {
            try { return Long.parseLong(s.trim()); } catch (NumberFormatException e) { return 0; }
        }
        throw new SQLiteException("Unable to convert BLOB to long");
    }

    public double getDouble(int row, int column) {
        Object v = value(row, column);
        if (v == null) return 0;
        if (v instanceof Long l) return l;
        if (v instanceof Double d) return d;
        if (v instanceof String s) {
            try { return Double.parseDouble(s.trim()); } catch (NumberFormatException e) { return 0; }
        }
        throw new SQLiteException("Unable to convert BLOB to double");
    }

    public short getShort(int row, int column) { return (short) getLong(row, column); }
    public int getInt(int row, int column) { return (int) getLong(row, column); }
    public float getFloat(int row, int column) { return (float) getDouble(row, column); }

    public boolean putBlob(byte[] value, int row, int column) { return put(value.clone(), row, column); }
    public boolean putString(String value, int row, int column) { return put(value, row, column); }
    public boolean putLong(long value, int row, int column) { return put(value, row, column); }
    public boolean putDouble(double value, int row, int column) { return put(value, row, column); }
    public boolean putNull(int row, int column) { return put(null, row, column); }

    @Override protected void onAllReferencesReleased() { clear(); }
    @Override public String toString() { return getName() + " {" + rows.size() + " rows}"; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { acquireReference(); dest.writeValue(this); }

    public static final Parcelable.Creator<CursorWindow> CREATOR = new Parcelable.Creator<>() {
        @Override public CursorWindow createFromParcel(Parcel in) { return (CursorWindow) in.readValue(null); }
        @Override public CursorWindow[] newArray(int size) { return new CursorWindow[size]; }
    };
}
