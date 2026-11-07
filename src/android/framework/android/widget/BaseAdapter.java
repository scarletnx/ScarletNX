// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.database.DataSetObserver;
import android.view.View;
import android.view.ViewGroup;
import java.util.ArrayList;

/** The common part of a list and spinner adapter: its observers. */
public abstract class BaseAdapter implements ListAdapter, SpinnerAdapter {
    private final ArrayList<DataSetObserver> observers = new ArrayList<>();
    private CharSequence[] autofillOptions;

    public BaseAdapter() {}

    @Override public boolean hasStableIds() { return false; }
    @Override public void registerDataSetObserver(DataSetObserver observer) {
        if (observer == null) throw new IllegalArgumentException("The observer is null.");
        synchronized (observers) {
            if (observers.contains(observer)) throw new IllegalStateException("Observer " + observer + " is already registered.");
            observers.add(observer);
        }
    }
    @Override public void unregisterDataSetObserver(DataSetObserver observer) {
        if (observer == null) throw new IllegalArgumentException("The observer is null.");
        synchronized (observers) {
            if (!observers.remove(observer)) throw new IllegalStateException("Observer " + observer + " was not registered.");
        }
    }
    private DataSetObserver[] observers() { synchronized (observers) { return observers.toArray(new DataSetObserver[0]); } }
    public void notifyDataSetChanged() { DataSetObserver[] o = observers(); for (int i = o.length - 1; i >= 0; --i) o[i].onChanged(); }
    public void notifyDataSetInvalidated() { DataSetObserver[] o = observers(); for (int i = o.length - 1; i >= 0; --i) o[i].onInvalidated(); }
    @Override public boolean areAllItemsEnabled() { return true; }
    @Override public boolean isEnabled(int position) { return true; }
    @Override public View getDropDownView(int position, View convertView, ViewGroup parent) { return getView(position, convertView, parent); }
    @Override public int getItemViewType(int position) { return 0; }
    @Override public int getViewTypeCount() { return 1; }
    @Override public boolean isEmpty() { return getCount() == 0; }
    @Override public CharSequence[] getAutofillOptions() { return autofillOptions; }
    public void setAutofillOptions(CharSequence... options) { autofillOptions = options; }
}
