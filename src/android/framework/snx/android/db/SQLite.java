// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.db;

/** SQLite's C library, the HLE of libsqlite and its JNI (src/android/db/sqlite_jni.c). Handles are its pointers. */
public final class SQLite {
    public static final int OPEN_READONLY = 0x1, OPEN_READWRITE = 0x2, OPEN_CREATE = 0x4;
    public static final int ROW = 100, DONE = 101;
    public static final int INTEGER = 1, FLOAT = 2, TEXT = 3, BLOB = 4, NULL = 5;

    private SQLite() {}

    /** A database, or an exception with SQLite's message. */
    public static native long open(String path, int flags);
    public static native void close(long db);
    public static native long prepare(long db, String sql);
    public static native void finalizeStatement(long statement);
    public static native void reset(long statement);
    public static native void clearBindings(long statement);
    public static native int parameterCount(long statement);
    public static native void bindNull(long statement, int index);
    public static native void bindLong(long statement, int index, long value);
    public static native void bindDouble(long statement, int index, double value);
    public static native void bindString(long statement, int index, String value);
    public static native void bindBlob(long statement, int index, byte[] value);
    /** One step: ROW or DONE, or an exception with SQLite's message. */
    public static native int step(long statement);
    public static native int columnCount(long statement);
    public static native String columnName(long statement, int index);
    public static native int columnType(long statement, int index);
    public static native long columnLong(long statement, int index);
    public static native double columnDouble(long statement, int index);
    public static native String columnString(long statement, int index);
    public static native byte[] columnBlob(long statement, int index);
    public static native int changes(long db);
    public static native long lastInsertRowId(long db);
    public static native boolean readOnly(long statement);
}
