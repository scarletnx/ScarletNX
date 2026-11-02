// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database.sqlite;

import android.database.Cursor;

/** What makes a cursor of a query. */
public interface SQLiteCursorDriver {
    Cursor query(SQLiteDatabase.CursorFactory factory, String[] bindArgs);
    void cursorDeactivated();
    void cursorRequeried(Cursor cursor);
    void cursorClosed();
    void setBindArguments(String[] bindArgs);
}
