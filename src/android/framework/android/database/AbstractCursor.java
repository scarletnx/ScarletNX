// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.database;

import android.content.ContentResolver;
import android.net.Uri;
import android.os.Bundle;
import java.util.ArrayList;

/** A cursor's position and observers. */
public abstract class AbstractCursor implements Cursor {
    protected int mPos = -1;
    protected boolean mClosed;
    private final ArrayList<ContentObserver> contentObservers = new ArrayList<>();
    private final ArrayList<DataSetObserver> dataSetObservers = new ArrayList<>();
    private Uri notifyUri;
    private Bundle extras = Bundle.EMPTY;

    public abstract int getCount();
    public abstract String[] getColumnNames();
    public abstract String getString(int column);
    public abstract short getShort(int column);
    public abstract int getInt(int column);
    public abstract long getLong(int column);
    public abstract float getFloat(int column);
    public abstract double getDouble(int column);
    public abstract boolean isNull(int column);
    public int getType(int column) { return FIELD_TYPE_STRING; }
    public byte[] getBlob(int column) { throw new UnsupportedOperationException("getBlob is not supported"); }
    public boolean onMove(int oldPosition, int newPosition) { return true; }

    @Override public int getColumnCount() { return getColumnNames().length; }
    @Override public void deactivate() {}
    @Override public boolean requery() { return true; }
    @Override public boolean isClosed() { return mClosed; }
    @Override public void close() { mClosed = true; contentObservers.clear(); }
    @Override public void copyStringToBuffer(int columnIndex, CharArrayBuffer buffer) {
        String s = getString(columnIndex);
        char[] c = s != null ? s.toCharArray() : new char[0];
        if (buffer.data == null || buffer.data.length < c.length) buffer.data = c; else System.arraycopy(c, 0, buffer.data, 0, c.length);
        buffer.sizeCopied = c.length;
    }
    @Override public final int getPosition() { return mPos; }
    @Override public final boolean moveToPosition(int position) {
        int count = getCount();
        if (position >= count) { mPos = count; return false; }
        if (position < 0) { mPos = -1; return false; }
        if (position == mPos) return true;
        boolean ok = onMove(mPos, position);
        mPos = ok ? position : -1;
        return ok;
    }
    @Override public final boolean move(int offset) { return moveToPosition(mPos + offset); }
    @Override public final boolean moveToFirst() { return moveToPosition(0); }
    @Override public final boolean moveToLast() { return moveToPosition(getCount() - 1); }
    @Override public final boolean moveToNext() { return moveToPosition(mPos + 1); }
    @Override public final boolean moveToPrevious() { return moveToPosition(mPos - 1); }
    @Override public final boolean isFirst() { return mPos == 0 && getCount() != 0; }
    @Override public final boolean isLast() { int c = getCount(); return mPos == c - 1 && c != 0; }
    @Override public final boolean isBeforeFirst() { return getCount() == 0 || mPos == -1; }
    @Override public final boolean isAfterLast() { return getCount() == 0 || mPos == getCount(); }
    @Override public int getColumnIndex(String columnName) {
        int period = columnName.lastIndexOf('.');
        if (period != -1) columnName = columnName.substring(period + 1);
        String[] names = getColumnNames();
        for (int i = 0; i < names.length; ++i) if (names[i].equalsIgnoreCase(columnName)) return i;
        return -1;
    }
    @Override public int getColumnIndexOrThrow(String columnName) {
        int i = getColumnIndex(columnName);
        if (i < 0) throw new IllegalArgumentException("column '" + columnName + "' does not exist. Available columns: " + java.util.Arrays.toString(getColumnNames()));
        return i;
    }
    @Override public String getColumnName(int columnIndex) { return getColumnNames()[columnIndex]; }
    @Override public void registerContentObserver(ContentObserver observer) { contentObservers.add(observer); }
    @Override public void unregisterContentObserver(ContentObserver observer) { contentObservers.remove(observer); }
    @Override public void registerDataSetObserver(DataSetObserver observer) { dataSetObservers.add(observer); }
    @Override public void unregisterDataSetObserver(DataSetObserver observer) { dataSetObservers.remove(observer); }
    protected void onChange(boolean selfChange) { for (ContentObserver o : new ArrayList<>(contentObservers)) o.dispatchChange(selfChange, null); }
    @Override public void setNotificationUri(ContentResolver cr, Uri notifyUri) { this.notifyUri = notifyUri; }
    @Override public Uri getNotificationUri() { return notifyUri; }
    @Override public boolean getWantsAllOnMoveCalls() { return false; }
    @Override public void setExtras(Bundle extras) { this.extras = extras != null ? extras : Bundle.EMPTY; }
    @Override public Bundle getExtras() { return extras; }
    @Override public Bundle respond(Bundle extras) { return Bundle.EMPTY; }
    protected void checkPosition() {
        if (mPos == -1 || mPos == getCount()) throw new CursorIndexOutOfBoundsException(mPos, getCount());
    }
}
