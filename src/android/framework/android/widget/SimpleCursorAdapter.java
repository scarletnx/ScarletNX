// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.view.View;

/** Binds columns of a cursor to TextViews and ImageViews of a row layout. */
public class SimpleCursorAdapter extends ResourceCursorAdapter {
    protected int[] mFrom;
    protected int[] mTo;
    private String[] originalFrom;
    private int stringConversionColumn = -1;
    private CursorToStringConverter stringConverter;
    private ViewBinder viewBinder;

    public interface ViewBinder { boolean setViewValue(View view, Cursor cursor, int columnIndex); }
    public interface CursorToStringConverter { CharSequence convertToString(Cursor cursor); }

    @Deprecated public SimpleCursorAdapter(Context context, int layout, Cursor c, String[] from, int[] to) { super(context, layout, c); mTo = to; originalFrom = from; findColumns(c, from); }
    public SimpleCursorAdapter(Context context, int layout, Cursor c, String[] from, int[] to, int flags) { super(context, layout, c, flags); mTo = to; originalFrom = from; findColumns(c, from); }

    private void findColumns(Cursor c, String[] from) {
        if (c == null) { mFrom = null; return; }
        mFrom = new int[from.length];
        for (int i = 0; i < from.length; ++i) mFrom[i] = c.getColumnIndexOrThrow(from[i]);
    }

    @Override public void bindView(View view, Context context, Cursor cursor) {
        for (int i = 0; i < mTo.length; ++i) {
            View v = view.findViewById(mTo[i]);
            if (v == null) continue;
            if (viewBinder != null && viewBinder.setViewValue(v, cursor, mFrom[i])) continue;
            String text = cursor.getString(mFrom[i]);
            if (text == null) text = "";
            if (v instanceof TextView t) setViewText(t, text);
            else if (v instanceof ImageView img) setViewImage(img, text);
            else throw new IllegalStateException(v.getClass().getName() + " is not a view that can be bound by this SimpleCursorAdapter");
        }
    }
    public ViewBinder getViewBinder() { return viewBinder; }
    public void setViewBinder(ViewBinder viewBinder) { this.viewBinder = viewBinder; }
    public void setViewImage(ImageView v, String value) {
        try { v.setImageResource(Integer.parseInt(value)); }
        catch (NumberFormatException e) { v.setImageURI(Uri.parse(value)); }
    }
    public void setViewText(TextView v, String text) { v.setText(text); }
    public int getStringConversionColumn() { return stringConversionColumn; }
    public void setStringConversionColumn(int stringConversionColumn) { this.stringConversionColumn = stringConversionColumn; }
    public CursorToStringConverter getCursorToStringConverter() { return stringConverter; }
    public void setCursorToStringConverter(CursorToStringConverter cursorToStringConverter) { stringConverter = cursorToStringConverter; }
    @Override public CharSequence convertToString(Cursor cursor) {
        if (stringConverter != null) return stringConverter.convertToString(cursor);
        if (stringConversionColumn > -1) return cursor.getString(stringConversionColumn);
        return super.convertToString(cursor);
    }
    @Override public Cursor swapCursor(Cursor c) { findColumns(c, originalFrom); return super.swapCursor(c); }
    public void changeCursorAndColumns(Cursor c, String[] from, int[] to) { originalFrom = from; mTo = to; findColumns(c, originalFrom); super.changeCursor(c); }
}
