// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.util.AttributeSet;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewParent;

/** A view whose children an adapter makes. */
public abstract class AdapterView<T extends Adapter> extends ViewGroup {
    public static final int ITEM_VIEW_TYPE_IGNORE = -1;
    public static final int ITEM_VIEW_TYPE_HEADER_OR_FOOTER = -2;
    public static final int INVALID_POSITION = -1;
    public static final long INVALID_ROW_ID = Long.MIN_VALUE;

    int firstPosition;
    int selectedPosition = INVALID_POSITION;
    private View emptyView;
    private OnItemClickListener onItemClickListener;
    private OnItemLongClickListener onItemLongClickListener;
    private OnItemSelectedListener onItemSelectedListener;

    public interface OnItemClickListener { void onItemClick(AdapterView<?> parent, View view, int position, long id); }
    public interface OnItemLongClickListener { boolean onItemLongClick(AdapterView<?> parent, View view, int position, long id); }
    public interface OnItemSelectedListener {
        void onItemSelected(AdapterView<?> parent, View view, int position, long id);
        void onNothingSelected(AdapterView<?> parent);
    }

    public AdapterView(Context context) { super(context); }
    public AdapterView(Context context, AttributeSet attrs) { super(context, attrs); }
    public AdapterView(Context context, AttributeSet attrs, int defStyleAttr) { super(context, attrs, defStyleAttr); }
    public AdapterView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) { super(context, attrs, defStyleAttr, defStyleRes); }

    public abstract T getAdapter();
    public abstract void setAdapter(T adapter);
    public abstract View getSelectedView();
    public abstract void setSelection(int position);

    public void setOnItemClickListener(OnItemClickListener listener) { onItemClickListener = listener; }
    public final OnItemClickListener getOnItemClickListener() { return onItemClickListener; }
    public boolean performItemClick(View view, int position, long id) {
        if (onItemClickListener == null) return false;
        playSoundEffect(android.view.SoundEffectConstants.CLICK);
        onItemClickListener.onItemClick(this, view, position, id);
        return true;
    }
    public void setOnItemLongClickListener(OnItemLongClickListener listener) {
        if (!isLongClickable()) setLongClickable(true);
        onItemLongClickListener = listener;
    }
    public final OnItemLongClickListener getOnItemLongClickListener() { return onItemLongClickListener; }
    public void setOnItemSelectedListener(OnItemSelectedListener listener) { onItemSelectedListener = listener; }
    public final OnItemSelectedListener getOnItemSelectedListener() { return onItemSelectedListener; }

    /** Tells the selection listener of the item at the selected position, or that none is selected. */
    void fireSelected() {
        if (onItemSelectedListener == null) return;
        if (selectedPosition >= 0 && selectedPosition < getCount()) {
            View v = getSelectedView();
            onItemSelectedListener.onItemSelected(this, v, selectedPosition, getAdapter().getItemId(selectedPosition));
        } else {
            onItemSelectedListener.onNothingSelected(this);
        }
    }

    @Override public void addView(View child) { throw new UnsupportedOperationException("addView(View) is not supported in AdapterView"); }
    @Override public void addView(View child, int index) { throw new UnsupportedOperationException("addView(View, int) is not supported in AdapterView"); }
    @Override public void addView(View child, LayoutParams params) { throw new UnsupportedOperationException("addView(View, LayoutParams) is not supported in AdapterView"); }
    @Override public void addView(View child, int index, LayoutParams params) { throw new UnsupportedOperationException("addView(View, int, LayoutParams) is not supported in AdapterView"); }
    @Override public void removeView(View child) { throw new UnsupportedOperationException("removeView(View) is not supported in AdapterView"); }
    @Override public void removeViewAt(int index) { throw new UnsupportedOperationException("removeViewAt(int) is not supported in AdapterView"); }
    @Override public void removeAllViews() { throw new UnsupportedOperationException("removeAllViews() is not supported in AdapterView"); }
    @Override public void setOnClickListener(OnClickListener l) {
        throw new RuntimeException("Don't call setOnClickListener for an AdapterView. You probably want setOnItemClickListener instead");
    }

    public int getSelectedItemPosition() { return selectedPosition; }
    public long getSelectedItemId() {
        T a = getAdapter();
        return a != null && selectedPosition >= 0 && selectedPosition < a.getCount() ? a.getItemId(selectedPosition) : INVALID_ROW_ID;
    }
    public Object getSelectedItem() {
        T a = getAdapter();
        return a != null && selectedPosition >= 0 && selectedPosition < a.getCount() ? a.getItem(selectedPosition) : null;
    }
    public int getCount() { T a = getAdapter(); return a != null ? a.getCount() : 0; }
    public int getPositionForView(View view) {
        View v = view;
        for (ViewParent p = v.getParent(); p != this; p = v.getParent()) {
            if (!(p instanceof View pv)) return INVALID_POSITION;
            v = pv;
        }
        int i = indexOfChild(v);
        return i >= 0 ? firstPosition + i : INVALID_POSITION;
    }
    public int getFirstVisiblePosition() { return firstPosition; }
    public int getLastVisiblePosition() { return firstPosition + getChildCount() - 1; }
    public Object getItemAtPosition(int position) { T a = getAdapter(); return a == null || position < 0 ? null : a.getItem(position); }
    public long getItemIdAtPosition(int position) { T a = getAdapter(); return a == null || position < 0 ? INVALID_ROW_ID : a.getItemId(position); }

    public void setEmptyView(View emptyView) { this.emptyView = emptyView; updateEmptyStatus(); }
    public View getEmptyView() { return emptyView; }
    /** With no items, the empty view shows in place of this one. */
    void updateEmptyStatus() {
        T a = getAdapter();
        boolean empty = a == null || a.isEmpty();
        if (emptyView == null) { setVisibility(VISIBLE); return; }
        emptyView.setVisibility(empty ? VISIBLE : GONE);
        setVisibility(empty ? GONE : VISIBLE);
    }

    @Override public CharSequence getAccessibilityClassName() { return AdapterView.class.getName(); }
}
