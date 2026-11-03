// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database;

import android.content.ContentResolver;
import android.net.Uri;
import android.os.Bundle;
import java.util.List;

/** A cursor that passes each call to the cursor it wraps. */
public class CursorWrapper implements Cursor {
    protected final Cursor mCursor;

    public CursorWrapper(Cursor cursor) { mCursor = cursor; }

    public Cursor getWrappedCursor() { return mCursor; }

    @Override public void close() { mCursor.close(); }
    @Override public boolean isClosed() { return mCursor.isClosed(); }
    @Override public int getCount() { return mCursor.getCount(); }
    @Override @Deprecated public void deactivate() { mCursor.deactivate(); }
    @Override public boolean moveToFirst() { return mCursor.moveToFirst(); }
    @Override public int getColumnCount() { return mCursor.getColumnCount(); }
    @Override public int getColumnIndex(String columnName) { return mCursor.getColumnIndex(columnName); }
    @Override public int getColumnIndexOrThrow(String columnName) throws IllegalArgumentException { return mCursor.getColumnIndexOrThrow(columnName); }
    @Override public String getColumnName(int columnIndex) { return mCursor.getColumnName(columnIndex); }
    @Override public String[] getColumnNames() { return mCursor.getColumnNames(); }
    @Override public double getDouble(int columnIndex) { return mCursor.getDouble(columnIndex); }
    @Override public void setExtras(Bundle extras) { mCursor.setExtras(extras); }
    @Override public Bundle getExtras() { return mCursor.getExtras(); }
    @Override public float getFloat(int columnIndex) { return mCursor.getFloat(columnIndex); }
    @Override public int getInt(int columnIndex) { return mCursor.getInt(columnIndex); }
    @Override public long getLong(int columnIndex) { return mCursor.getLong(columnIndex); }
    @Override public short getShort(int columnIndex) { return mCursor.getShort(columnIndex); }
    @Override public String getString(int columnIndex) { return mCursor.getString(columnIndex); }
    @Override public void copyStringToBuffer(int columnIndex, CharArrayBuffer buffer) { mCursor.copyStringToBuffer(columnIndex, buffer); }
    @Override public byte[] getBlob(int columnIndex) { return mCursor.getBlob(columnIndex); }
    @Override public boolean getWantsAllOnMoveCalls() { return mCursor.getWantsAllOnMoveCalls(); }
    @Override public boolean isAfterLast() { return mCursor.isAfterLast(); }
    @Override public boolean isBeforeFirst() { return mCursor.isBeforeFirst(); }
    @Override public boolean isFirst() { return mCursor.isFirst(); }
    @Override public boolean isLast() { return mCursor.isLast(); }
    @Override public int getType(int columnIndex) { return mCursor.getType(columnIndex); }
    @Override public boolean isNull(int columnIndex) { return mCursor.isNull(columnIndex); }
    @Override public boolean moveToLast() { return mCursor.moveToLast(); }
    @Override public boolean move(int offset) { return mCursor.move(offset); }
    @Override public boolean moveToPosition(int position) { return mCursor.moveToPosition(position); }
    @Override public boolean moveToNext() { return mCursor.moveToNext(); }
    @Override public int getPosition() { return mCursor.getPosition(); }
    @Override public boolean moveToPrevious() { return mCursor.moveToPrevious(); }
    @Override public void registerContentObserver(ContentObserver observer) { mCursor.registerContentObserver(observer); }
    @Override public void registerDataSetObserver(DataSetObserver observer) { mCursor.registerDataSetObserver(observer); }
    @Override @Deprecated public boolean requery() { return mCursor.requery(); }
    @Override public Bundle respond(Bundle extras) { return mCursor.respond(extras); }
    @Override public void setNotificationUri(ContentResolver cr, Uri uri) { mCursor.setNotificationUri(cr, uri); }
    @Override public void setNotificationUris(ContentResolver cr, List<Uri> uris) { mCursor.setNotificationUris(cr, uris); }
    @Override public Uri getNotificationUri() { return mCursor.getNotificationUri(); }
    @Override public List<Uri> getNotificationUris() { return mCursor.getNotificationUris(); }
    @Override public void unregisterContentObserver(ContentObserver observer) { mCursor.unregisterContentObserver(observer); }
    @Override public void unregisterDataSetObserver(DataSetObserver observer) { mCursor.unregisterDataSetObserver(observer); }
}
