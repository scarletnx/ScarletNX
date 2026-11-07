// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database;

/** Database helpers. */
public class DatabaseUtils {
    public static final int STATEMENT_SELECT = 1, STATEMENT_UPDATE = 2, STATEMENT_ATTACH = 3, STATEMENT_BEGIN = 4,
        STATEMENT_COMMIT = 5, STATEMENT_ABORT = 6, STATEMENT_PRAGMA = 7, STATEMENT_DDL = 8, STATEMENT_UNPREPARED = 9, STATEMENT_OTHER = 99;
    public static int getSqlStatementType(String sql) {
        String s = sql.trim();
        if (s.length() < 3) return STATEMENT_OTHER;
        String p = s.substring(0, 3).toUpperCase(java.util.Locale.ROOT);
        switch (p) {
            case "SEL": return STATEMENT_SELECT;
            case "INS": case "UPD": case "REP": case "DEL": return STATEMENT_UPDATE;
            case "ATT": return STATEMENT_ATTACH;
            case "COM": case "END": return STATEMENT_COMMIT;
            case "ROL": return s.toUpperCase(java.util.Locale.ROOT).contains(" TO ") ? STATEMENT_OTHER : STATEMENT_ABORT;
            case "BEG": return STATEMENT_BEGIN;
            case "PRA": return STATEMENT_PRAGMA;
            case "CRE": case "DRO": case "ALT": return STATEMENT_DDL;
            case "ANA": case "DET": return STATEMENT_UNPREPARED;
            default: return STATEMENT_OTHER;
        }
    }
    public static long queryNumEntries(android.database.sqlite.SQLiteDatabase db, String table) { return longForQuery(db, "SELECT COUNT(*) FROM " + table, null); }
    public static long longForQuery(android.database.sqlite.SQLiteDatabase db, String query, String[] selectionArgs) {
        try (android.database.sqlite.SQLiteStatement s = db.compileStatement(query)) {
            if (selectionArgs != null) for (int i = 0; i < selectionArgs.length; ++i) s.bindString(i + 1, selectionArgs[i]);
            return s.simpleQueryForLong();
        }
    }
    public static String stringForQuery(android.database.sqlite.SQLiteDatabase db, String query, String[] selectionArgs) {
        try (android.database.sqlite.SQLiteStatement s = db.compileStatement(query)) {
            if (selectionArgs != null) for (int i = 0; i < selectionArgs.length; ++i) s.bindString(i + 1, selectionArgs[i]);
            return s.simpleQueryForString();
        }
    }
    public static void appendEscapedSQLString(StringBuilder sb, String sqlString) { sb.append('\'').append(sqlString.replace("'", "''")).append('\''); }
    public static String sqlEscapeString(String value) { StringBuilder b = new StringBuilder(); appendEscapedSQLString(b, value); return b.toString(); }
    public static void cursorRowToContentValues(Cursor cursor, android.content.ContentValues values) {
        String[] columns = cursor.getColumnNames();
        for (int i = 0; i < columns.length; ++i) values.put(columns[i], cursor.getString(i));
    }
}
