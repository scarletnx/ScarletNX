// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.database.Cursor;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;

/** A cursor adapter that inflates a layout resource for each row. */
public abstract class ResourceCursorAdapter extends CursorAdapter {
    private int layout, dropDownLayout;
    private final LayoutInflater inflater;

    @Deprecated public ResourceCursorAdapter(Context context, int layout, Cursor c) { super(context, c); inflater = LayoutInflater.from(context); this.layout = dropDownLayout = layout; }
    @Deprecated public ResourceCursorAdapter(Context context, int layout, Cursor c, boolean autoRequery) { super(context, c, autoRequery); inflater = LayoutInflater.from(context); this.layout = dropDownLayout = layout; }
    public ResourceCursorAdapter(Context context, int layout, Cursor c, int flags) { super(context, c, flags); inflater = LayoutInflater.from(context); this.layout = dropDownLayout = layout; }

    @Override public View newView(Context context, Cursor cursor, ViewGroup parent) { return inflater.inflate(layout, parent, false); }
    @Override public View newDropDownView(Context context, Cursor cursor, ViewGroup parent) { return inflater.inflate(dropDownLayout, parent, false); }
    public void setViewResource(int layout) { this.layout = layout; }
    public void setDropDownViewResource(int dropDownLayout) { this.dropDownLayout = dropDownLayout; }
}
