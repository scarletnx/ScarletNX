// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;
import android.util.SparseArray;
import android.view.View;
import android.view.ViewGroup;
import java.util.ArrayDeque;
import java.util.ArrayList;

/** A vertical list: the items of the adapter, one under the other, from a first position and an offset. */
public class ListView extends AbsListView {
    private final ArrayList<FixedViewInfo> headers = new ArrayList<>(), footers = new ArrayList<>();
    private ListAdapter userAdapter;
    private Drawable divider;
    private int dividerHeight;
    private boolean headerDividers = true, footerDividers = true, itemsCanFocus;
    private int firstTop;
    private final SparseArray<ArrayDeque<View>> scrap = new SparseArray<>();

    public class FixedViewInfo {
        public View view;
        public Object data;
        public boolean isSelectable;
        public FixedViewInfo() {}
    }

    public ListView(Context context) { super(context); }
    public ListView(Context context, AttributeSet attrs) { super(context, attrs); }
    public ListView(Context context, AttributeSet attrs, int defStyleAttr) { super(context, attrs, defStyleAttr); }
    public ListView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) { super(context, attrs, defStyleAttr, defStyleRes); }

    public void addHeaderView(View v, Object data, boolean isSelectable) { headers.add(info(v, data, isSelectable)); wrap(); }
    public void addHeaderView(View v) { addHeaderView(v, null, true); }
    public int getHeaderViewsCount() { return headers.size(); }
    public boolean removeHeaderView(View v) { return remove(headers, v); }
    public void addFooterView(View v, Object data, boolean isSelectable) { footers.add(info(v, data, isSelectable)); wrap(); }
    public void addFooterView(View v) { addFooterView(v, null, true); }
    public int getFooterViewsCount() { return footers.size(); }
    public boolean removeFooterView(View v) { return remove(footers, v); }
    private FixedViewInfo info(View v, Object data, boolean selectable) { FixedViewInfo i = new FixedViewInfo(); i.view = v; i.data = data; i.isSelectable = selectable; return i; }
    private boolean remove(ArrayList<FixedViewInfo> list, View v) {
        for (int i = 0; i < list.size(); ++i) if (list.get(i).view == v) { list.remove(i); wrap(); return true; }
        return false;
    }
    /** With header or footer views, the list shows the adapter through a HeaderViewListAdapter. */
    private void wrap() {
        if (userAdapter == null) return;
        ListAdapter a = headers.isEmpty() && footers.isEmpty() ? userAdapter : new HeaderViewListAdapter(headers, footers, userAdapter);
        if (a != mAdapter) super.setAdapter(a); else { requestLayout(); invalidate(); }
    }
    @Override public ListAdapter getAdapter() { return mAdapter; }
    @Override public void setAdapter(ListAdapter adapter) {
        userAdapter = adapter;
        scrap.clear();
        firstTop = 0;
        super.setAdapter(adapter == null || headers.isEmpty() && footers.isEmpty() ? adapter : new HeaderViewListAdapter(headers, footers, adapter));
    }
    @Override public void setSelection(int position) {
        if (mAdapter == null || position < 0 || position >= mAdapter.getCount()) return;
        firstPosition = position;
        firstTop = 0;
        selectedPosition = position;
        requestLayout();
        invalidate();
        fireSelected();
    }
    public void setDivider(Drawable divider) { this.divider = divider; if (divider != null && divider.getIntrinsicHeight() > 0) dividerHeight = divider.getIntrinsicHeight(); requestLayout(); }
    public Drawable getDivider() { return divider; }
    public void setDividerHeight(int height) { dividerHeight = height; requestLayout(); }
    public int getDividerHeight() { return dividerHeight; }
    public void setHeaderDividersEnabled(boolean enabled) { headerDividers = enabled; }
    public boolean areHeaderDividersEnabled() { return headerDividers; }
    public void setFooterDividersEnabled(boolean enabled) { footerDividers = enabled; }
    public boolean areFooterDividersEnabled() { return footerDividers; }
    public void setItemsCanFocus(boolean itemsCanFocus) { this.itemsCanFocus = itemsCanFocus; }
    public boolean getItemsCanFocus() { return itemsCanFocus; }

    private View obtain(int position) {
        int type = mAdapter.getItemViewType(position);
        ArrayDeque<View> q = type >= 0 ? scrap.get(type) : null;
        View v = mAdapter.getView(position, q != null ? q.poll() : null, this);
        ViewGroup.LayoutParams p = v.getLayoutParams();
        LayoutParams lp = p == null ? (LayoutParams) generateDefaultLayoutParams() : p instanceof LayoutParams l ? l : new LayoutParams(p);
        lp.viewType = type;
        v.setLayoutParams(lp);
        return v;
    }
    private void recycle(View v) {
        int type = ((LayoutParams) v.getLayoutParams()).viewType;
        if (type < 0) return;
        ArrayDeque<View> q = scrap.get(type);
        if (q == null) scrap.put(type, q = new ArrayDeque<>());
        q.add(v);
    }
    private void measureItem(View v, int widthSpec) {
        ViewGroup.LayoutParams lp = v.getLayoutParams();
        int ws = getChildMeasureSpec(widthSpec, getPaddingLeft() + getPaddingRight(), lp.width);
        int hs = lp.height > 0 ? MeasureSpec.makeMeasureSpec(lp.height, MeasureSpec.EXACTLY) : MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED);
        v.measure(ws, hs);
    }
    private int itemHeight(int position) {
        View v = obtain(position);
        measureItem(v, MeasureSpec.makeMeasureSpec(getWidth(), MeasureSpec.EXACTLY));
        int h = v.getMeasuredHeight();
        recycle(v);
        return h;
    }

    @Override protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int count = mAdapter != null ? mAdapter.getCount() : 0;
        int wmode = MeasureSpec.getMode(widthMeasureSpec), hmode = MeasureSpec.getMode(heightMeasureSpec);
        int width = MeasureSpec.getSize(widthMeasureSpec), height = MeasureSpec.getSize(heightMeasureSpec);
        int childWidth = 0, total = getPaddingTop() + getPaddingBottom();
        if (count > 0 && (wmode == MeasureSpec.UNSPECIFIED || hmode != MeasureSpec.EXACTLY)) {
            int limit = hmode == MeasureSpec.AT_MOST ? height : Integer.MAX_VALUE;
            for (int i = 0; i < count && total < limit; ++i) {
                View v = obtain(i);
                measureItem(v, wmode == MeasureSpec.UNSPECIFIED ? widthMeasureSpec : MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY));
                childWidth = Math.max(childWidth, v.getMeasuredWidth());
                total += v.getMeasuredHeight() + (i > 0 ? dividerHeight : 0);
                recycle(v);
                if (hmode == MeasureSpec.UNSPECIFIED) break;
            }
        }
        if (wmode == MeasureSpec.UNSPECIFIED) width = childWidth + getPaddingLeft() + getPaddingRight();
        if (hmode == MeasureSpec.AT_MOST) height = Math.min(total, height);
        else if (hmode == MeasureSpec.UNSPECIFIED) height = total;
        setMeasuredDimension(Math.max(width, getSuggestedMinimumWidth()), Math.max(height, getSuggestedMinimumHeight()));
    }

    @Override protected void onLayout(boolean changed, int l, int t, int r, int b) { layoutChildren(); }

    /** Places the items that the list shows, from the first position and its offset, and keeps the list filled to its end. */
    protected void layoutChildren() {
        for (int i = 0; i < getChildCount(); ++i) recycle(getChildAt(i));
        removeAllViewsInLayout();
        int count = mAdapter != null ? mAdapter.getCount() : 0;
        if (count == 0) { firstPosition = 0; firstTop = 0; reportScroll(); return; }
        int top = getPaddingTop(), bottom = getHeight() - getPaddingBottom();
        firstPosition = Math.max(0, Math.min(firstPosition, count - 1));
        while (firstTop > 0 && firstPosition > 0) firstTop -= itemHeight(--firstPosition) + dividerHeight;
        while (firstPosition < count - 1) {
            int h = itemHeight(firstPosition) + dividerHeight;
            if (firstTop + h > 0) break;
            firstTop += h;
            ++firstPosition;
        }
        if (firstPosition == 0 && firstTop > 0) firstTop = 0;
        int widthSpec = MeasureSpec.makeMeasureSpec(getWidth(), MeasureSpec.EXACTLY);
        int y = top + firstTop, pos = firstPosition;
        for (; pos < count && y < bottom; ++pos) {
            View v = obtain(pos);
            measureItem(v, widthSpec);
            place(v, -1, y);
            y += v.getMeasuredHeight() + dividerHeight;
        }
        int gap = bottom - (y - dividerHeight);
        if (pos == count && gap > 0 && (firstPosition > 0 || firstTop < 0)) {
            for (int i = 0; i < getChildCount(); ++i) getChildAt(i).offsetTopAndBottom(gap);
            while (getChildAt(0).getTop() > top && firstPosition > 0) {
                View v = obtain(--firstPosition);
                measureItem(v, widthSpec);
                place(v, 0, getChildAt(0).getTop() - dividerHeight - v.getMeasuredHeight());
            }
            int over = getChildAt(0).getTop() - top;
            if (firstPosition == 0 && over > 0) for (int i = 0; i < getChildCount(); ++i) getChildAt(i).offsetTopAndBottom(-over);
        }
        firstTop = getChildAt(0).getTop() - top;
        refreshChecked();
        reportScroll();
    }
    private void place(View v, int index, int y) {
        addViewInLayout(v, index, v.getLayoutParams(), true);
        int x = getPaddingLeft();
        v.layout(x, y, x + v.getMeasuredWidth(), y + v.getMeasuredHeight());
    }
    @Override void scrollContentBy(int dy) {
        if (dy == 0 || getChildCount() == 0) return;
        firstTop -= dy;
        layoutChildren();
        invalidate();
    }

    @Override protected void dispatchDraw(Canvas canvas) {
        super.dispatchDraw(canvas);
        if (divider == null || dividerHeight <= 0) return;
        for (int i = 0; i + 1 < getChildCount(); ++i) {
            int y = getChildAt(i).getBottom();
            divider.setBounds(getPaddingLeft(), y, getWidth() - getPaddingRight(), y + dividerHeight);
            divider.draw(canvas);
        }
    }
    @Override public CharSequence getAccessibilityClassName() { return ListView.class.getName(); }
}
