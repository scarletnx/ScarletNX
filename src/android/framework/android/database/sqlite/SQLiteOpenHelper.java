// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2007 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.database.sqlite;

import android.content.Context;
import android.database.DatabaseErrorHandler;

/** Opens a database at a version: created, upgraded or downgraded on the way. */
public abstract class SQLiteOpenHelper implements AutoCloseable {
    private final Context context;
    private final String name;
    private final int version, minimumSupportedVersion;
    private final SQLiteDatabase.CursorFactory factory;
    private final DatabaseErrorHandler errorHandler;
    private SQLiteDatabase database;
    private boolean initializing, wal;
    private SQLiteDatabase.OpenParams params;

    public SQLiteOpenHelper(Context context, String name, SQLiteDatabase.CursorFactory factory, int version) { this(context, name, factory, version, null); }
    public SQLiteOpenHelper(Context context, String name, SQLiteDatabase.CursorFactory factory, int version, DatabaseErrorHandler errorHandler) {
        this(context, name, factory, version, 0, errorHandler);
    }
    public SQLiteOpenHelper(Context context, String name, int version, SQLiteDatabase.OpenParams openParams) {
        this(context, name, null, version, 0, openParams.getErrorHandler());
        params = openParams;
    }
    public SQLiteOpenHelper(Context context, String name, SQLiteDatabase.CursorFactory factory, int version, int minimumSupportedVersion,
                            DatabaseErrorHandler errorHandler) {
        if (version < 1) throw new IllegalArgumentException("Version must be >= 1, was " + version);
        this.context = context;
        this.name = name;
        this.factory = factory;
        this.version = version;
        this.minimumSupportedVersion = Math.max(0, minimumSupportedVersion);
        this.errorHandler = errorHandler;
    }

    public String getDatabaseName() { return name; }
    public void setWriteAheadLoggingEnabled(boolean enabled) {
        synchronized (this) {
            wal = enabled;
            if (database != null && database.isOpen()) { if (enabled) database.enableWriteAheadLogging(); else database.disableWriteAheadLogging(); }
        }
    }
    public void setLookasideConfig(int slotSize, int slotCount) {}
    public void setOpenParams(SQLiteDatabase.OpenParams openParams) { params = openParams; }
    public void setIdleConnectionTimeout(long idleConnectionTimeoutMs) {}
    public SQLiteDatabase getWritableDatabase() { synchronized (this) { return getDatabaseLocked(true); } }
    public SQLiteDatabase getReadableDatabase() { synchronized (this) { return getDatabaseLocked(false); } }

    private SQLiteDatabase getDatabaseLocked(boolean writable) {
        if (database != null && database.isOpen()) return database;
        if (initializing) throw new IllegalStateException("getDatabase called recursively");
        SQLiteDatabase db = null;
        try {
            initializing = true;
            if (name == null) {
                db = SQLiteDatabase.create(null);
            } else {
                String path = context.getDatabasePath(name).getPath();
                db = SQLiteDatabase.openDatabase(path, factory, SQLiteDatabase.CREATE_IF_NECESSARY, errorHandler);
            }
            onConfigure(db);
            if (wal) db.enableWriteAheadLogging();
            int current = db.getVersion();
            if (current != version) {
                if (current > 0 && current < minimumSupportedVersion) {
                    db.close();
                    context.deleteDatabase(name);
                    db = SQLiteDatabase.openDatabase(context.getDatabasePath(name).getPath(), factory, SQLiteDatabase.CREATE_IF_NECESSARY, errorHandler);
                    onConfigure(db);
                    current = 0;
                }
                db.beginTransaction();
                try {
                    if (current == 0) onCreate(db);
                    else if (current > version) onDowngrade(db, current, version);
                    else onUpgrade(db, current, version);
                    db.setVersion(version);
                    db.setTransactionSuccessful();
                } finally {
                    db.endTransaction();
                }
            }
            onOpen(db);
            database = db;
            return db;
        } finally {
            initializing = false;
            if (db != null && db != database) db.close();
        }
    }

    @Override public synchronized void close() {
        if (initializing) throw new IllegalStateException("Closed during initialization");
        if (database != null && database.isOpen()) { database.close(); database = null; }
    }
    public void onConfigure(SQLiteDatabase db) {}
    public void onBeforeDelete(SQLiteDatabase db) {}
    public abstract void onCreate(SQLiteDatabase db);
    public abstract void onUpgrade(SQLiteDatabase db, int oldVersion, int newVersion);
    public void onDowngrade(SQLiteDatabase db, int oldVersion, int newVersion) {
        throw new SQLiteException("Can't downgrade database from version " + oldVersion + " to " + newVersion);
    }
    public void onOpen(SQLiteDatabase db) {}
}
