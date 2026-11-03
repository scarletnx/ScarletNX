// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.database.DataSetObserver;
import android.view.View;
import android.view.ViewGroup;

/** The items of an adapter view, and the view of each one. */
public interface Adapter {
    int IGNORE_ITEM_VIEW_TYPE = AdapterView.ITEM_VIEW_TYPE_IGNORE;
    int NO_SELECTION = Integer.MIN_VALUE;

    void registerDataSetObserver(DataSetObserver observer);
    void unregisterDataSetObserver(DataSetObserver observer);
    int getCount();
    Object getItem(int position);
    long getItemId(int position);
    boolean hasStableIds();
    View getView(int position, View convertView, ViewGroup parent);
    int getItemViewType(int position);
    int getViewTypeCount();
    boolean isEmpty();
    default CharSequence[] getAutofillOptions() { return null; }
}
