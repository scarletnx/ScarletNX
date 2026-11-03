// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.database.DataSetObserver;
import android.os.Parcel;
import android.os.Parcelable;
import android.util.AttributeSet;
import android.view.View;
import android.view.ViewGroup;

/** An adapter view that shows the view of its selected item. */
public abstract class AbsSpinner extends AdapterView<SpinnerAdapter> {
    SpinnerAdapter mAdapter;
    private final DataSetObserver observer = new DataSetObserver() {
        @Override public void onChanged() { dataChanged(); }
        @Override public void onInvalidated() { dataChanged(); }
    };

    public AbsSpinner(Context context) { super(context); }
    public AbsSpinner(Context context, AttributeSet attrs) { super(context, attrs); }
    public AbsSpinner(Context context, AttributeSet attrs, int defStyleAttr) { super(context, attrs, defStyleAttr); }
    public AbsSpinner(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) { super(context, attrs, defStyleAttr, defStyleRes); }

    @Override public void setAdapter(SpinnerAdapter adapter) {
        if (mAdapter != null) mAdapter.unregisterDataSetObserver(observer);
        mAdapter = adapter;
        if (adapter != null) adapter.registerDataSetObserver(observer);
        selectedPosition = adapter != null && adapter.getCount() > 0 ? 0 : INVALID_POSITION;
        removeAllViewsInLayout();
        updateEmptyStatus();
        requestLayout();
        fireSelected();
    }
    private void dataChanged() {
        int count = getCount();
        int old = selectedPosition;
        if (count == 0) selectedPosition = INVALID_POSITION;
        else if (selectedPosition < 0 || selectedPosition >= count) selectedPosition = Math.max(0, Math.min(count - 1, selectedPosition));
        updateEmptyStatus();
        requestLayout();
        invalidate();
        if (old != selectedPosition) fireSelected();
    }
    @Override public SpinnerAdapter getAdapter() { return mAdapter; }
    @Override public int getCount() { return mAdapter != null ? mAdapter.getCount() : 0; }
    @Override public void setSelection(int position) { setSelection(position, false); }
    public void setSelection(int position, boolean animate) {
        if (position < 0 || position >= getCount() || position == selectedPosition) return;
        selectedPosition = position;
        requestLayout();
        invalidate();
        fireSelected();
    }
    @Override public View getSelectedView() { return getChildCount() > 0 ? getChildAt(0) : null; }
    public int pointToPosition(int x, int y) {
        View c = getSelectedView();
        return c != null && x >= c.getLeft() && x < c.getRight() && y >= c.getTop() && y < c.getBottom() ? selectedPosition : INVALID_POSITION;
    }

    /** The view of the selected item, made again for each layout; the old view is given back to the adapter. */
    View selectedItemView() {
        View old = getChildCount() > 0 ? getChildAt(0) : null;
        removeAllViewsInLayout();
        if (mAdapter == null || selectedPosition < 0 || selectedPosition >= mAdapter.getCount()) return null;
        View v = mAdapter.getView(selectedPosition, old, this);
        ViewGroup.LayoutParams lp = v.getLayoutParams();
        addViewInLayout(v, 0, lp != null ? lp : generateDefaultLayoutParams(), true);
        return v;
    }
    @Override protected ViewGroup.LayoutParams generateDefaultLayoutParams() { return new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT); }

    static class SavedState extends BaseSavedState {
        final int position;
        SavedState(Parcelable superState, int position) { super(superState); this.position = position; }
        SavedState(Parcel in) { super(in); position = in.readInt(); }
        @Override public void writeToParcel(Parcel out, int flags) { super.writeToParcel(out, flags); out.writeInt(position); }
        public static final Parcelable.Creator<SavedState> CREATOR = new Parcelable.Creator<SavedState>() {
            @Override public SavedState createFromParcel(Parcel in) { return new SavedState(in); }
            @Override public SavedState[] newArray(int size) { return new SavedState[size]; }
        };
    }
    @Override public Parcelable onSaveInstanceState() { return new SavedState(super.onSaveInstanceState(), selectedPosition); }
    @Override public void onRestoreInstanceState(Parcelable state) {
        if (!(state instanceof SavedState s)) { super.onRestoreInstanceState(state); return; }
        super.onRestoreInstanceState(s.getSuperState());
        if (s.position >= 0) setSelection(s.position);
    }
    @Override public CharSequence getAccessibilityClassName() { return AbsSpinner.class.getName(); }
}
