// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.database;

/** A cursor whose rows are in a CursorWindow. */
public abstract class AbstractWindowedCursor extends AbstractCursor implements CrossProcessCursor {
    protected CursorWindow mWindow;

    public AbstractWindowedCursor() {}

    @Override public abstract int getCount();

    @Override public byte[] getBlob(int columnIndex) { checkPosition(); return mWindow.getBlob(mPos, columnIndex); }
    @Override public String getString(int columnIndex) { checkPosition(); return mWindow.getString(mPos, columnIndex); }
    @Override public void copyStringToBuffer(int columnIndex, CharArrayBuffer buffer) { checkPosition(); mWindow.copyStringToBuffer(mPos, columnIndex, buffer); }
    @Override public short getShort(int columnIndex) { checkPosition(); return mWindow.getShort(mPos, columnIndex); }
    @Override public int getInt(int columnIndex) { checkPosition(); return mWindow.getInt(mPos, columnIndex); }
    @Override public long getLong(int columnIndex) { checkPosition(); return mWindow.getLong(mPos, columnIndex); }
    @Override public float getFloat(int columnIndex) { checkPosition(); return mWindow.getFloat(mPos, columnIndex); }
    @Override public double getDouble(int columnIndex) { checkPosition(); return mWindow.getDouble(mPos, columnIndex); }
    @Override public boolean isNull(int columnIndex) { checkPosition(); return mWindow.isNull(mPos, columnIndex); }
    @Override public int getType(int columnIndex) { checkPosition(); return mWindow.getType(mPos, columnIndex); }

    @Override protected void checkPosition() {
        super.checkPosition();
        if (mWindow == null) throw new StaleDataException("Attempting to access a closed CursorWindow.");
    }

    @Override public CursorWindow getWindow() { return mWindow; }

    public void setWindow(CursorWindow window) {
        if (window == mWindow) return;
        if (mWindow != null) mWindow.close();
        mWindow = window;
    }

    public boolean hasWindow() { return mWindow != null; }

    @Override public boolean onMove(int oldPosition, int newPosition) { return true; }

    /** The rows from position on, as many as the window takes. */
    @Override public void fillWindow(int position, CursorWindow window) {
        if (position < 0 || position >= getCount()) return;
        int oldPos = getPosition();
        int columns = getColumnCount();
        window.clear();
        window.setStartPosition(position);
        window.setNumColumns(columns);
        if (moveToPosition(position)) {
            do {
                if (!window.allocRow()) break;
                int row = getPosition();
                boolean ok = true;
                for (int i = 0; i < columns && ok; ++i) {
                    switch (getType(i)) {
                        case FIELD_TYPE_NULL -> ok = window.putNull(row, i);
                        case FIELD_TYPE_INTEGER -> ok = window.putLong(getLong(i), row, i);
                        case FIELD_TYPE_FLOAT -> ok = window.putDouble(getDouble(i), row, i);
                        case FIELD_TYPE_BLOB -> { byte[] b = getBlob(i); ok = b != null ? window.putBlob(b, row, i) : window.putNull(row, i); }
                        default -> { String s = getString(i); ok = s != null ? window.putString(s, row, i) : window.putNull(row, i); }
                    }
                }
                if (!ok) { window.freeLastRow(); break; }
            } while (moveToNext());
        }
        moveToPosition(oldPos);
    }

    @Override public void close() {
        super.close();
        setWindow(null);
    }
}
