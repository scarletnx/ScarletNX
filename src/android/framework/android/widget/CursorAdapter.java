// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.widget;

import android.content.Context;
import android.database.ContentObserver;
import android.database.Cursor;
import android.database.DataSetObserver;
import android.os.Handler;
import android.view.View;
import android.view.ViewGroup;

/** An adapter over the rows of a cursor, which must have an _id column. */
public abstract class CursorAdapter extends BaseAdapter implements Filterable {
    @Deprecated public static final int FLAG_AUTO_REQUERY = 0x01;
    public static final int FLAG_REGISTER_CONTENT_OBSERVER = 0x02;

    protected boolean mDataValid;
    protected boolean mAutoRequery;
    protected Cursor mCursor;
    protected Context mContext;
    protected int mRowIDColumn;
    private boolean observe;
    private FilterQueryProvider filterQueryProvider;
    private Filter filter;
    private final ContentObserver changeObserver = new ContentObserver(new Handler()) {
        @Override public boolean deliverSelfNotifications() { return true; }
        @Override public void onChange(boolean selfChange) { onContentChanged(); }
    };
    private final DataSetObserver dataSetObserver = new DataSetObserver() {
        @Override public void onChanged() { mDataValid = true; notifyDataSetChanged(); }
        @Override public void onInvalidated() { mDataValid = false; notifyDataSetInvalidated(); }
    };

    @Deprecated public CursorAdapter(Context context, Cursor c) { init(context, c, FLAG_AUTO_REQUERY); }
    public CursorAdapter(Context context, Cursor c, boolean autoRequery) { init(context, c, autoRequery ? FLAG_AUTO_REQUERY : FLAG_REGISTER_CONTENT_OBSERVER); }
    public CursorAdapter(Context context, Cursor c, int flags) { init(context, c, flags); }

    @Deprecated protected void init(Context context, Cursor c, boolean autoRequery) { init(context, c, autoRequery ? FLAG_AUTO_REQUERY : FLAG_REGISTER_CONTENT_OBSERVER); }
    void init(Context context, Cursor c, int flags) {
        mAutoRequery = (flags & FLAG_AUTO_REQUERY) != 0;
        observe = mAutoRequery || (flags & FLAG_REGISTER_CONTENT_OBSERVER) != 0;
        mContext = context;
        mCursor = c;
        mDataValid = c != null;
        mRowIDColumn = c != null ? c.getColumnIndexOrThrow("_id") : -1;
        if (c != null) register(c);
    }
    private void register(Cursor c) { if (observe) c.registerContentObserver(changeObserver); c.registerDataSetObserver(dataSetObserver); }
    private void unregister(Cursor c) { if (observe) c.unregisterContentObserver(changeObserver); c.unregisterDataSetObserver(dataSetObserver); }

    public Cursor getCursor() { return mCursor; }
    @Override public int getCount() { return mDataValid && mCursor != null ? mCursor.getCount() : 0; }
    @Override public Object getItem(int position) {
        if (!mDataValid || mCursor == null) return null;
        mCursor.moveToPosition(position);
        return mCursor;
    }
    @Override public long getItemId(int position) { return mDataValid && mCursor != null && mCursor.moveToPosition(position) ? mCursor.getLong(mRowIDColumn) : 0; }
    @Override public boolean hasStableIds() { return true; }
    @Override public View getView(int position, View convertView, ViewGroup parent) {
        if (!mDataValid) throw new IllegalStateException("this should only be called when the cursor is valid");
        if (!mCursor.moveToPosition(position)) throw new IllegalStateException("couldn't move cursor to position " + position);
        View v = convertView != null ? convertView : newView(mContext, mCursor, parent);
        bindView(v, mContext, mCursor);
        return v;
    }
    @Override public View getDropDownView(int position, View convertView, ViewGroup parent) {
        if (!mDataValid) return null;
        mCursor.moveToPosition(position);
        View v = convertView != null ? convertView : newDropDownView(mContext, mCursor, parent);
        bindView(v, mContext, mCursor);
        return v;
    }
    public abstract View newView(Context context, Cursor cursor, ViewGroup parent);
    public View newDropDownView(Context context, Cursor cursor, ViewGroup parent) { return newView(context, cursor, parent); }
    public abstract void bindView(View view, Context context, Cursor cursor);

    public void changeCursor(Cursor cursor) { Cursor old = swapCursor(cursor); if (old != null) old.close(); }
    public Cursor swapCursor(Cursor newCursor) {
        if (newCursor == mCursor) return null;
        Cursor old = mCursor;
        if (old != null) unregister(old);
        mCursor = newCursor;
        if (newCursor != null) {
            register(newCursor);
            mRowIDColumn = newCursor.getColumnIndexOrThrow("_id");
            mDataValid = true;
            notifyDataSetChanged();
        } else {
            mRowIDColumn = -1;
            mDataValid = false;
            notifyDataSetInvalidated();
        }
        return old;
    }
    public CharSequence convertToString(Cursor cursor) { return cursor == null ? "" : cursor.toString(); }
    public Cursor runQueryOnBackgroundThread(CharSequence constraint) { return filterQueryProvider != null ? filterQueryProvider.runQuery(constraint) : mCursor; }
    @Override public Filter getFilter() {
        if (filter == null) filter = new Filter() {
            @Override public CharSequence convertResultToString(Object resultValue) { return convertToString((Cursor) resultValue); }
            @Override protected FilterResults performFiltering(CharSequence constraint) {
                Cursor c = runQueryOnBackgroundThread(constraint);
                FilterResults r = new FilterResults();
                if (c != null) { r.count = c.getCount(); r.values = c; }
                return r;
            }
            @Override protected void publishResults(CharSequence constraint, FilterResults results) {
                if (results.values != null && results.values != mCursor) changeCursor((Cursor) results.values);
            }
        };
        return filter;
    }
    public FilterQueryProvider getFilterQueryProvider() { return filterQueryProvider; }
    public void setFilterQueryProvider(FilterQueryProvider filterQueryProvider) { this.filterQueryProvider = filterQueryProvider; }
    @SuppressWarnings("deprecation")
    protected void onContentChanged() { if (mAutoRequery && mCursor != null && !mCursor.isClosed()) mDataValid = mCursor.requery(); }
}
