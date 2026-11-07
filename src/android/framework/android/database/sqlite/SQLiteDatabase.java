// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database.sqlite;

import android.content.ContentValues;
import android.database.Cursor;
import android.database.DatabaseErrorHandler;
import android.database.DefaultDatabaseErrorHandler;
import android.os.CancellationSignal;
import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.locks.ReentrantLock;
import snx.android.db.SQLite;

/**
 * A SQLite database on one connection. Its lock stands in for Android's connection pool: a transaction holds it
 * until it ends, so another thread's statements wait for it, as for the one primary connection.
 */
public final class SQLiteDatabase extends SQLiteClosable {
    public static final int CONFLICT_ROLLBACK = 1, CONFLICT_ABORT = 2, CONFLICT_FAIL = 3, CONFLICT_IGNORE = 4,
        CONFLICT_REPLACE = 5, CONFLICT_NONE = 0;
    public static final int OPEN_READWRITE = 0, OPEN_READONLY = 1, NO_LOCALIZED_COLLATORS = 0x10, CREATE_IF_NECESSARY = 0x10000000,
        ENABLE_WRITE_AHEAD_LOGGING = 0x20000000;
    public static final int MAX_SQL_CACHE_SIZE = 100, SQLITE_MAX_LIKE_PATTERN_LENGTH = 50000;
    public static final String JOURNAL_MODE_WAL = "WAL", JOURNAL_MODE_DELETE = "DELETE", SYNC_MODE_NORMAL = "NORMAL",
        SYNC_MODE_FULL = "FULL";
    private static final String[] CONFLICT = {"", " OR ROLLBACK ", " OR ABORT ", " OR FAIL ", " OR IGNORE ", " OR REPLACE "};

    public interface CursorFactory {
        Cursor newCursor(SQLiteDatabase db, SQLiteCursorDriver masterQuery, String editTable, SQLiteQuery query);
    }

    private final String path;
    private final int flags;
    private final DatabaseErrorHandler errorHandler;
    private long handle;
    private final ReentrantLock lock = new ReentrantLock(true);
    private int depth;
    private boolean markedSuccessful, childFailed;
    private final ArrayList<SQLiteTransactionListener> listeners = new ArrayList<>();
    private boolean wal;

    private SQLiteDatabase(String path, int flags, DatabaseErrorHandler errorHandler) {
        this.path = path;
        this.flags = flags;
        this.errorHandler = errorHandler != null ? errorHandler : new DefaultDatabaseErrorHandler();
    }

    private void open() {
        int f = (flags & OPEN_READONLY) != 0 ? SQLite.OPEN_READONLY : SQLite.OPEN_READWRITE;
        if ((flags & CREATE_IF_NECESSARY) != 0) f |= SQLite.OPEN_CREATE;
        if (!path.equals(":memory:")) new File(path).getAbsoluteFile().getParentFile().mkdirs();
        try {
            handle = SQLite.open(path, f);
        } catch (RuntimeException e) {
            throw new SQLiteCantOpenDatabaseException("Cannot open database '" + path + "': " + e.getMessage(), e);
        }
    }

    long handle() {
        if (handle == 0) throw new IllegalStateException("attempt to re-open an already-closed object: SQLiteDatabase: " + path);
        return handle;
    }

    interface Use<T> { T on(long statement); }

    /** A statement prepared, its arguments bound, used, finalized: under the database's lock. */
    <T> T withStatement(String sql, Object[] args, Use<T> use) {
        lock.lock();
        try {
            long s;
            try {
                s = SQLite.prepare(handle(), sql);
            } catch (RuntimeException e) {
                throw translate(e, sql);
            }
            try {
                if (args != null) {
                    for (int i = 0; i < args.length; ++i) {
                        Object a = args[i];
                        if (a == null) SQLite.bindNull(s, i + 1);
                        else if (a instanceof byte[] b) SQLite.bindBlob(s, i + 1, b);
                        else if (a instanceof Double || a instanceof Float) SQLite.bindDouble(s, i + 1, ((Number) a).doubleValue());
                        else if (a instanceof Number n) SQLite.bindLong(s, i + 1, n.longValue());
                        else if (a instanceof Boolean b) SQLite.bindLong(s, i + 1, b ? 1 : 0);
                        else SQLite.bindString(s, i + 1, a.toString());
                    }
                }
                return use.on(s);
            } catch (SQLiteException e) {
                throw e;
            } catch (RuntimeException e) {
                throw translate(e, sql);
            } finally {
                SQLite.finalizeStatement(s);
            }
        } finally {
            lock.unlock();
        }
    }

    /** A statement stepped until it is done, then use: an update's or insert's result. */
    <T> T run(String sql, Object[] args, Use<T> use) {
        return withStatement(sql, args, s -> {
            while (SQLite.step(s) == SQLite.ROW) {}
            return use.on(s);
        });
    }

    private static SQLiteException translate(RuntimeException e, String sql) {
        String m = String.valueOf(e.getMessage());
        String msg = m + ", while compiling: " + sql;
        if (m.contains("constraint failed") || m.contains("UNIQUE") || m.contains("FOREIGN KEY")) return new SQLiteConstraintException(msg);
        if (m.contains("database is locked")) return new SQLiteDatabaseLockedException(msg);
        if (m.contains("disk I/O")) return new SQLiteDiskIOException(msg);
        if (m.contains("malformed") || m.contains("not a database")) return new SQLiteDatabaseCorruptException(msg);
        if (m.contains("readonly")) return new SQLiteReadOnlyDatabaseException(msg);
        if (m.contains("full")) return new SQLiteFullException(msg);
        return new SQLiteException(msg, e);
    }

    // ---- opening ----
    public static SQLiteDatabase openDatabase(String path, CursorFactory factory, int flags) { return openDatabase(path, factory, flags, null); }
    public static SQLiteDatabase openDatabase(String path, CursorFactory factory, int flags, DatabaseErrorHandler errorHandler) {
        SQLiteDatabase db = new SQLiteDatabase(path, flags, errorHandler);
        db.open();
        return db;
    }
    public static SQLiteDatabase openDatabase(File path, OpenParams openParams) {
        return openDatabase(path.getPath(), null, openParams.openFlags | CREATE_IF_NECESSARY, openParams.errorHandler);
    }
    public static SQLiteDatabase openOrCreateDatabase(File file, CursorFactory factory) { return openOrCreateDatabase(file.getPath(), factory); }
    public static SQLiteDatabase openOrCreateDatabase(String path, CursorFactory factory) { return openDatabase(path, factory, CREATE_IF_NECESSARY); }
    public static SQLiteDatabase openOrCreateDatabase(String path, CursorFactory factory, DatabaseErrorHandler errorHandler) {
        return openDatabase(path, factory, CREATE_IF_NECESSARY, errorHandler);
    }
    public static SQLiteDatabase create(CursorFactory factory) { return openDatabase(":memory:", factory, CREATE_IF_NECESSARY); }
    public static SQLiteDatabase createInMemory(OpenParams openParams) { return openDatabase(":memory:", null, CREATE_IF_NECESSARY); }
    public static boolean deleteDatabase(File file) {
        boolean deleted = file.delete();
        for (String suffix : new String[] {"-journal", "-shm", "-wal"}) deleted |= new File(file.getPath() + suffix).delete();
        return deleted;
    }
    public static int releaseMemory() { return 0; }

    public static final class OpenParams {
        final int openFlags;
        final DatabaseErrorHandler errorHandler;

        private OpenParams(Builder b) { openFlags = b.flags; errorHandler = b.errorHandler; }
        public int getOpenFlags() { return openFlags; }
        public DatabaseErrorHandler getErrorHandler() { return errorHandler; }

        public static final class Builder {
            private int flags;
            private DatabaseErrorHandler errorHandler;
            public Builder() {}
            public Builder(OpenParams params) { flags = params.openFlags; errorHandler = params.errorHandler; }
            public Builder setOpenFlags(int openFlags) { flags = openFlags; return this; }
            public Builder addOpenFlags(int openFlags) { flags |= openFlags; return this; }
            public Builder removeOpenFlags(int openFlags) { flags &= ~openFlags; return this; }
            public Builder setErrorHandler(DatabaseErrorHandler errorHandler) { this.errorHandler = errorHandler; return this; }
            public Builder setCursorFactory(CursorFactory cursorFactory) { return this; }
            public Builder setLookasideConfig(int slotSize, int slotCount) { return this; }
            public Builder setIdleConnectionTimeout(long idleConnectionTimeoutMs) { return this; }
            public Builder setJournalMode(String journalMode) { return this; }
            public Builder setSynchronousMode(String syncMode) { return this; }
            public OpenParams build() { return new OpenParams(this); }
        }
    }

    // ---- state ----
    public String getPath() { return path; }
    public boolean isOpen() { return handle != 0; }
    public boolean isReadOnly() { return (flags & OPEN_READONLY) != 0; }
    public boolean isInMemoryDatabase() { return path.equals(":memory:"); }
    public int getVersion() { return (int) longForQuery("PRAGMA user_version;"); }
    public void setVersion(int version) { execSQL("PRAGMA user_version = " + version); }
    public long getMaximumSize() { return longForQuery("PRAGMA max_page_count;") * getPageSize(); }
    public long setMaximumSize(long numBytes) { return getMaximumSize(); }
    public long getPageSize() { return longForQuery("PRAGMA page_size;"); }
    public void setPageSize(long numBytes) { execSQL("PRAGMA page_size = " + numBytes); }
    public boolean needUpgrade(int newVersion) { return newVersion > getVersion(); }
    public void setLocale(Locale locale) {}
    public void setMaxSqlCacheSize(int cacheSize) {}
    public void setForeignKeyConstraintsEnabled(boolean enable) { execSQL("PRAGMA foreign_keys = " + (enable ? 1 : 0)); }
    public boolean enableWriteAheadLogging() {
        if (isInMemoryDatabase() || isReadOnly()) return false;
        try (Cursor c = rawQuery("PRAGMA journal_mode = WAL", null)) { wal = c.moveToFirst() && "wal".equalsIgnoreCase(c.getString(0)); }
        return wal;
    }
    public void disableWriteAheadLogging() {
        try (Cursor c = rawQuery("PRAGMA journal_mode = DELETE", null)) { c.moveToFirst(); }
        wal = false;
    }
    public boolean isWriteAheadLoggingEnabled() { return wal; }
    public List<android.util.Pair<String, String>> getAttachedDbs() {
        ArrayList<android.util.Pair<String, String>> out = new ArrayList<>();
        try (Cursor c = rawQuery("PRAGMA database_list;", null)) {
            while (c.moveToNext()) out.add(new android.util.Pair<>(c.getString(1), c.getString(2)));
        }
        return out;
    }
    public boolean isDatabaseIntegrityOk() {
        try (Cursor c = rawQuery("PRAGMA integrity_check(1);", null)) { return c.moveToFirst() && "ok".equalsIgnoreCase(c.getString(0)); }
    }
    public boolean isDbLockedByCurrentThread() { return lock.isHeldByCurrentThread(); }
    @Deprecated public boolean isDbLockedByOtherThreads() { return lock.isLocked() && !lock.isHeldByCurrentThread(); }
    private long longForQuery(String sql) { return new SQLiteStatement(this, sql, null).simpleQueryForLong(); }
    @Override protected void onAllReferencesReleased() {
        lock.lock();
        try {
            if (handle != 0) SQLite.close(handle);
            handle = 0;
        } finally {
            lock.unlock();
        }
    }

    // ---- transactions ----
    public void beginTransaction() { begin(null, "BEGIN EXCLUSIVE;"); }
    public void beginTransactionNonExclusive() { begin(null, "BEGIN IMMEDIATE;"); }
    public void beginTransactionReadOnly() { begin(null, "BEGIN DEFERRED;"); }
    public void beginTransactionWithListener(SQLiteTransactionListener listener) { begin(listener, "BEGIN EXCLUSIVE;"); }
    public void beginTransactionWithListenerNonExclusive(SQLiteTransactionListener listener) { begin(listener, "BEGIN IMMEDIATE;"); }
    public void beginTransactionWithListenerReadOnly(SQLiteTransactionListener listener) { begin(listener, "BEGIN DEFERRED;"); }

    private void begin(SQLiteTransactionListener listener, String sql) {
        lock.lock();
        boolean ok = false;
        try {
            if (depth == 0) {
                run(sql, null, s -> null);
                markedSuccessful = false;
                childFailed = false;
            }
            markedSuccessful = false;
            ++depth;
            listeners.add(listener);
            if (listener != null) listener.onBegin();
            ok = true;
        } finally {
            if (!ok) lock.unlock();
        }
    }
    public void setTransactionSuccessful() {
        if (depth == 0) throw new IllegalStateException("Cannot perform this operation because there is no current transaction.");
        if (markedSuccessful) throw new IllegalStateException("Cannot perform this operation because the transaction has already been marked successful.");
        markedSuccessful = true;
    }
    public void endTransaction() {
        if (depth == 0 || !lock.isHeldByCurrentThread()) throw new IllegalStateException("Cannot perform this operation because there is no current transaction.");
        try {
            SQLiteTransactionListener listener = listeners.remove(listeners.size() - 1);
            boolean success = markedSuccessful && !childFailed;
            if (listener != null) { if (success) listener.onCommit(); else listener.onRollback(); }
            if (!markedSuccessful) childFailed = true;
            markedSuccessful = false;
            if (--depth == 0) {
                run(success ? "COMMIT;" : "ROLLBACK;", null, s -> null);
            } else {
                // the parent goes on; it commits only when every child succeeded
                markedSuccessful = false;
            }
        } finally {
            lock.unlock();
        }
    }
    public boolean inTransaction() { return depth > 0 && lock.isHeldByCurrentThread(); }
    @Deprecated public boolean yieldIfContended() { return false; }
    public boolean yieldIfContendedSafely() { return false; }
    public boolean yieldIfContendedSafely(long sleepAfterYieldDelay) { return false; }

    // ---- statements ----
    public SQLiteStatement compileStatement(String sql) { return new SQLiteStatement(this, sql, null); }
    public void execSQL(String sql) { run(sql, null, s -> null); }
    public void execSQL(String sql, Object[] bindArgs) { run(sql, bindArgs, s -> null); }
    public void execPerConnectionSQL(String sql, Object[] bindArgs) { execSQL(sql, bindArgs); }
    public void validateSql(String sql, CancellationSignal cancellationSignal) { withStatement(sql, null, s -> null); }

    public Cursor rawQuery(String sql, String[] selectionArgs) { return rawQueryWithFactory(null, sql, selectionArgs, null, null); }
    public Cursor rawQuery(String sql, String[] selectionArgs, CancellationSignal cancellationSignal) { return rawQueryWithFactory(null, sql, selectionArgs, null, cancellationSignal); }
    public Cursor rawQueryWithFactory(CursorFactory factory, String sql, String[] selectionArgs, String editTable) {
        return rawQueryWithFactory(factory, sql, selectionArgs, editTable, null);
    }
    public Cursor rawQueryWithFactory(CursorFactory factory, String sql, String[] selectionArgs, String editTable, CancellationSignal cancellationSignal) {
        SQLiteQuery query = new SQLiteQuery(this, sql, selectionArgs);
        SQLiteCursorDriver driver = new SQLiteCursorDriver() {
            @Override public Cursor query(CursorFactory f, String[] bindArgs) { return null; }
            @Override public void cursorDeactivated() {}
            @Override public void cursorRequeried(Cursor cursor) {}
            @Override public void cursorClosed() {}
            @Override public void setBindArguments(String[] bindArgs) {}
        };
        return factory != null ? factory.newCursor(this, driver, editTable, query) : new SQLiteCursor(driver, editTable, query);
    }
    public Cursor query(boolean distinct, String table, String[] columns, String selection, String[] selectionArgs, String groupBy,
                        String having, String orderBy, String limit) {
        return rawQuery(SQLiteQueryBuilder.buildQueryString(distinct, table, columns, selection, groupBy, having, orderBy, limit), selectionArgs);
    }
    public Cursor query(boolean distinct, String table, String[] columns, String selection, String[] selectionArgs, String groupBy,
                        String having, String orderBy, String limit, CancellationSignal cancellationSignal) {
        return query(distinct, table, columns, selection, selectionArgs, groupBy, having, orderBy, limit);
    }
    public Cursor queryWithFactory(CursorFactory cursorFactory, boolean distinct, String table, String[] columns, String selection,
                                   String[] selectionArgs, String groupBy, String having, String orderBy, String limit) {
        return rawQueryWithFactory(cursorFactory, SQLiteQueryBuilder.buildQueryString(distinct, table, columns, selection, groupBy, having, orderBy, limit),
            selectionArgs, table);
    }
    public Cursor query(String table, String[] columns, String selection, String[] selectionArgs, String groupBy, String having, String orderBy) {
        return query(false, table, columns, selection, selectionArgs, groupBy, having, orderBy, null);
    }
    public Cursor query(String table, String[] columns, String selection, String[] selectionArgs, String groupBy, String having, String orderBy, String limit) {
        return query(false, table, columns, selection, selectionArgs, groupBy, having, orderBy, limit);
    }
    public long insert(String table, String nullColumnHack, ContentValues values) {
        try { return insertWithOnConflict(table, nullColumnHack, values, CONFLICT_NONE); } catch (android.database.SQLException e) { return -1; }
    }
    public long insertOrThrow(String table, String nullColumnHack, ContentValues values) { return insertWithOnConflict(table, nullColumnHack, values, CONFLICT_NONE); }
    public long replace(String table, String nullColumnHack, ContentValues initialValues) {
        try { return insertWithOnConflict(table, nullColumnHack, initialValues, CONFLICT_REPLACE); } catch (android.database.SQLException e) { return -1; }
    }
    public long replaceOrThrow(String table, String nullColumnHack, ContentValues initialValues) { return insertWithOnConflict(table, nullColumnHack, initialValues, CONFLICT_REPLACE); }
    public long insertWithOnConflict(String table, String nullColumnHack, ContentValues initialValues, int conflictAlgorithm) {
        StringBuilder sql = new StringBuilder("INSERT").append(CONFLICT[conflictAlgorithm]).append(" INTO ").append(table).append('(');
        int size = initialValues != null ? initialValues.size() : 0;
        Object[] args = null;
        if (size > 0) {
            args = new Object[size];
            int i = 0;
            for (String col : initialValues.keySet()) {
                sql.append(i > 0 ? "," : "").append(col);
                args[i++] = initialValues.get(col);
            }
            sql.append(')').append(" VALUES (");
            for (i = 0; i < size; ++i) sql.append(i > 0 ? ",?" : "?");
        } else {
            sql.append(nullColumnHack).append(") VALUES (NULL");
        }
        sql.append(')');
        return new SQLiteStatement(this, sql.toString(), args).executeInsert();
    }
    public int delete(String table, String whereClause, String[] whereArgs) {
        return new SQLiteStatement(this, "DELETE FROM " + table + (whereClause != null && !whereClause.isEmpty() ? " WHERE " + whereClause : ""), whereArgs).executeUpdateDelete();
    }
    public int update(String table, ContentValues values, String whereClause, String[] whereArgs) {
        return updateWithOnConflict(table, values, whereClause, whereArgs, CONFLICT_NONE);
    }
    public int updateWithOnConflict(String table, ContentValues values, String whereClause, String[] whereArgs, int conflictAlgorithm) {
        if (values == null || values.isEmpty()) throw new IllegalArgumentException("Empty values");
        StringBuilder sql = new StringBuilder("UPDATE").append(CONFLICT[conflictAlgorithm]).append(' ').append(table).append(" SET ");
        int setValuesSize = values.size(), bindArgsSize = whereArgs == null ? setValuesSize : setValuesSize + whereArgs.length;
        Object[] args = new Object[bindArgsSize];
        int i = 0;
        for (String col : values.keySet()) {
            sql.append(i > 0 ? "," : "").append(col);
            args[i++] = values.get(col);
            sql.append("=?");
        }
        if (whereArgs != null) for (i = setValuesSize; i < bindArgsSize; ++i) args[i] = whereArgs[i - setValuesSize];
        if (whereClause != null && !whereClause.isEmpty()) sql.append(" WHERE ").append(whereClause);
        return new SQLiteStatement(this, sql.toString(), args).executeUpdateDelete();
    }
    public static String findEditTable(String tables) {
        int spacepos = tables.indexOf(' '), commapos = tables.indexOf(',');
        if (spacepos > 0 && (spacepos < commapos || commapos < 0)) return tables.substring(0, spacepos);
        if (commapos > 0 && (commapos < spacepos || spacepos < 0)) return tables.substring(0, commapos);
        return tables;
    }
    @Override public String toString() { return "SQLiteDatabase: " + path; }
}
