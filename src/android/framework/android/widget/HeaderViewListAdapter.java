// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.database.DataSetObserver;
import android.view.View;
import android.view.ViewGroup;
import java.util.ArrayList;

/** A list adapter with header views before the items of another adapter, and footer views after them. */
public class HeaderViewListAdapter implements WrapperListAdapter, Filterable {
    private final ListAdapter adapter;
    private final ArrayList<ListView.FixedViewInfo> headers, footers;
    private final boolean isFilterable;

    public HeaderViewListAdapter(ArrayList<ListView.FixedViewInfo> headerViewInfos, ArrayList<ListView.FixedViewInfo> footerViewInfos, ListAdapter adapter) {
        this.adapter = adapter;
        isFilterable = adapter instanceof Filterable;
        headers = headerViewInfos != null ? headerViewInfos : new ArrayList<>();
        footers = footerViewInfos != null ? footerViewInfos : new ArrayList<>();
    }

    public int getHeadersCount() { return headers.size(); }
    public int getFootersCount() { return footers.size(); }
    private int inner() { return adapter != null ? adapter.getCount() : 0; }
    @Override public boolean isEmpty() { return adapter == null || adapter.isEmpty(); }
    public boolean removeHeader(View v) { return headers.removeIf(i -> i.view == v); }
    public boolean removeFooter(View v) { return footers.removeIf(i -> i.view == v); }
    @Override public int getCount() { return headers.size() + inner() + footers.size(); }
    @Override public boolean areAllItemsEnabled() {
        for (ListView.FixedViewInfo i : headers) if (!i.isSelectable) return false;
        for (ListView.FixedViewInfo i : footers) if (!i.isSelectable) return false;
        return adapter == null || adapter.areAllItemsEnabled();
    }
    @Override public boolean isEnabled(int position) {
        int h = headers.size();
        if (position < h) return headers.get(position).isSelectable;
        int p = position - h;
        if (p < inner()) return adapter.isEnabled(p);
        return footers.get(p - inner()).isSelectable;
    }
    @Override public Object getItem(int position) {
        int h = headers.size();
        if (position < h) return headers.get(position).data;
        int p = position - h;
        if (p < inner()) return adapter.getItem(p);
        return footers.get(p - inner()).data;
    }
    @Override public long getItemId(int position) {
        int p = position - headers.size();
        return p >= 0 && p < inner() ? adapter.getItemId(p) : -1;
    }
    @Override public boolean hasStableIds() { return adapter != null && adapter.hasStableIds(); }
    @Override public View getView(int position, View convertView, ViewGroup parent) {
        int h = headers.size();
        if (position < h) return headers.get(position).view;
        int p = position - h;
        if (p < inner()) return adapter.getView(p, convertView, parent);
        return footers.get(p - inner()).view;
    }
    @Override public int getItemViewType(int position) {
        int p = position - headers.size();
        return p >= 0 && p < inner() ? adapter.getItemViewType(p) : AdapterView.ITEM_VIEW_TYPE_HEADER_OR_FOOTER;
    }
    @Override public int getViewTypeCount() { return adapter != null ? adapter.getViewTypeCount() : 1; }
    @Override public void registerDataSetObserver(DataSetObserver observer) { if (adapter != null) adapter.registerDataSetObserver(observer); }
    @Override public void unregisterDataSetObserver(DataSetObserver observer) { if (adapter != null) adapter.unregisterDataSetObserver(observer); }
    @Override public Filter getFilter() { return isFilterable ? ((Filterable) adapter).getFilter() : null; }
    @Override public ListAdapter getWrappedAdapter() { return adapter; }
}
