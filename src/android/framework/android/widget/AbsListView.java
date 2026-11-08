// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.database.DataSetObserver;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.text.Editable;
import android.text.TextWatcher;
import android.util.AttributeSet;
import android.util.SparseBooleanArray;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;

/** A scrolling list of the views of an adapter. ListView places them; this class scrolls and clicks them. */
public abstract class AbsListView extends AdapterView<ListAdapter> implements TextWatcher, ViewTreeObserver.OnGlobalLayoutListener,
        Filter.FilterListener, ViewTreeObserver.OnTouchModeChangeListener {
    public static final int CHOICE_MODE_NONE = 0, CHOICE_MODE_SINGLE = 1, CHOICE_MODE_MULTIPLE = 2, CHOICE_MODE_MULTIPLE_MODAL = 3;
    public static final int TRANSCRIPT_MODE_DISABLED = 0, TRANSCRIPT_MODE_NORMAL = 1, TRANSCRIPT_MODE_ALWAYS_SCROLL = 2;

    ListAdapter mAdapter;
    private int choiceMode = CHOICE_MODE_NONE;
    private final SparseBooleanArray checkStates = new SparseBooleanArray();
    private OnScrollListener onScrollListener;
    private Drawable selector;
    private boolean drawSelectorOnTop, stackFromBottom, fastScroll, textFilter, smoothScrollbar = true, scrollingCache;
    private int transcriptMode, cacheColorHint;
    private final int touchSlop;
    private float downX, downY, lastY;
    private boolean dragging;
    private final DataSetObserver dataSetObserver = new DataSetObserver() {
        @Override public void onChanged() { updateEmptyStatus(); requestLayout(); invalidate(); }
        @Override public void onInvalidated() { updateEmptyStatus(); requestLayout(); invalidate(); }
    };

    public interface OnScrollListener {
        int SCROLL_STATE_IDLE = 0, SCROLL_STATE_TOUCH_SCROLL = 1, SCROLL_STATE_FLING = 2;
        void onScrollStateChanged(AbsListView view, int scrollState);
        void onScroll(AbsListView view, int firstVisibleItem, int visibleItemCount, int totalItemCount);
    }
    public interface SelectionBoundsAdjuster { void adjustListItemSelectionBounds(Rect bounds); }
    public interface RecyclerListener { void onMovedToScrapHeap(View view); }

    public static class LayoutParams extends ViewGroup.LayoutParams {
        int viewType;
        public LayoutParams(Context c, AttributeSet attrs) { super(c, attrs); }
        public LayoutParams(int w, int h) { super(w, h); }
        public LayoutParams(int w, int h, int viewType) { super(w, h); this.viewType = viewType; }
        public LayoutParams(ViewGroup.LayoutParams source) { super(source); }
    }

    public AbsListView(Context context) { this(context, null); }
    public AbsListView(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public AbsListView(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public AbsListView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        touchSlop = ViewConfiguration.get(context).getScaledTouchSlop();
        setFocusableInTouchMode(true);
        setWillNotDraw(false);
    }

    @Override public ListAdapter getAdapter() { return mAdapter; }
    @Override public void setAdapter(ListAdapter adapter) {
        if (mAdapter != null) mAdapter.unregisterDataSetObserver(dataSetObserver);
        mAdapter = adapter;
        if (adapter != null) adapter.registerDataSetObserver(dataSetObserver);
        checkStates.clear();
        firstPosition = 0;
        selectedPosition = INVALID_POSITION;
        updateEmptyStatus();
        requestLayout();
        invalidate();
    }
    @Override public View getSelectedView() {
        int i = selectedPosition - firstPosition;
        return i >= 0 && i < getChildCount() ? getChildAt(i) : null;
    }

    public void setChoiceMode(int choiceMode) { this.choiceMode = choiceMode; checkStates.clear(); }
    public int getChoiceMode() { return choiceMode; }
    public void setItemChecked(int position, boolean value) {
        if (choiceMode == CHOICE_MODE_NONE) return;
        if (choiceMode == CHOICE_MODE_SINGLE && value) checkStates.clear();
        if (value) checkStates.put(position, true); else checkStates.delete(position);
        refreshChecked();
    }
    public boolean isItemChecked(int position) { return choiceMode != CHOICE_MODE_NONE && checkStates.get(position); }
    public int getCheckedItemPosition() { return choiceMode == CHOICE_MODE_SINGLE && checkStates.size() == 1 ? checkStates.keyAt(0) : INVALID_POSITION; }
    public SparseBooleanArray getCheckedItemPositions() { return choiceMode != CHOICE_MODE_NONE ? checkStates : null; }
    public int getCheckedItemCount() {
        int n = 0;
        for (int i = 0; i < checkStates.size(); ++i) if (checkStates.valueAt(i)) ++n;
        return n;
    }
    public long[] getCheckedItemIds() {
        if (mAdapter == null || !mAdapter.hasStableIds()) return new long[0];
        long[] ids = new long[getCheckedItemCount()];
        for (int i = 0, n = 0; i < checkStates.size(); ++i) if (checkStates.valueAt(i)) ids[n++] = mAdapter.getItemId(checkStates.keyAt(i));
        return ids;
    }
    public void clearChoices() { checkStates.clear(); refreshChecked(); }
    /** A checkable item view shows its state checked; another view shows it as activated. */
    void refreshChecked() {
        if (choiceMode == CHOICE_MODE_NONE) return;
        for (int i = 0; i < getChildCount(); ++i) {
            View c = getChildAt(i);
            boolean on = checkStates.get(firstPosition + i);
            if (c instanceof Checkable ch) ch.setChecked(on); else c.setActivated(on);
        }
    }
    @Override public boolean performItemClick(View view, int position, long id) {
        if (choiceMode == CHOICE_MODE_SINGLE) setItemChecked(position, true);
        else if (choiceMode == CHOICE_MODE_MULTIPLE || choiceMode == CHOICE_MODE_MULTIPLE_MODAL) setItemChecked(position, !checkStates.get(position));
        return super.performItemClick(view, position, id);
    }

    public void setOnScrollListener(OnScrollListener l) { onScrollListener = l; if (l != null) reportScroll(); }
    void reportScroll() { if (onScrollListener != null) onScrollListener.onScroll(this, firstPosition, getChildCount(), getCount()); }
    public void setSelector(Drawable sel) { selector = sel; }
    public void setSelector(int resID) { setSelector(getContext().getDrawable(resID)); }
    public Drawable getSelector() { return selector; }
    public void setDrawSelectorOnTop(boolean onTop) { drawSelectorOnTop = onTop; }
    public boolean isDrawSelectorOnTop() { return drawSelectorOnTop; }
    public void setStackFromBottom(boolean stackFromBottom) { this.stackFromBottom = stackFromBottom; }
    public boolean isStackFromBottom() { return stackFromBottom; }
    public void setFastScrollEnabled(boolean enabled) { fastScroll = enabled; }
    public boolean isFastScrollEnabled() { return fastScroll; }
    public void setTextFilterEnabled(boolean textFilterEnabled) { textFilter = textFilterEnabled; }
    public boolean isTextFilterEnabled() { return textFilter; }
    public void setSmoothScrollbarEnabled(boolean enabled) { smoothScrollbar = enabled; }
    public boolean isSmoothScrollbarEnabled() { return smoothScrollbar; }
    public void setScrollingCacheEnabled(boolean enabled) { scrollingCache = enabled; }
    public boolean isScrollingCacheEnabled() { return scrollingCache; }
    public void setTranscriptMode(int mode) { transcriptMode = mode; }
    public int getTranscriptMode() { return transcriptMode; }
    public void setCacheColorHint(int color) { cacheColorHint = color; }
    public int getCacheColorHint() { return cacheColorHint; }
    public void setRecyclerListener(RecyclerListener listener) {}

    /** Scrolls the content by a number of pixels; a positive y shows the items below. */
    public void scrollListBy(int y) { scrollContentBy(y); }
    public boolean canScrollList(int direction) {
        int n = getChildCount();
        if (n == 0) return false;
        if (direction > 0) return firstPosition + n < getCount() || getChildAt(n - 1).getBottom() > getHeight() - getPaddingBottom();
        return firstPosition > 0 || getChildAt(0).getTop() < getPaddingTop();
    }
    public void smoothScrollToPosition(int position) { setSelection(position); }
    public void smoothScrollBy(int distance, int duration) { scrollListBy(distance); }
    public void setSelectionFromTop(int position, int y) { setSelection(position); }
    abstract void scrollContentBy(int dy);

    @Override public boolean onInterceptTouchEvent(MotionEvent ev) {
        switch (ev.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: downX = ev.getX(); downY = lastY = ev.getY(); dragging = false; return false;
            case MotionEvent.ACTION_MOVE:
                if (!dragging && Math.abs(ev.getY() - downY) > touchSlop) { dragging = true; lastY = ev.getY(); scrollStateChanged(OnScrollListener.SCROLL_STATE_TOUCH_SCROLL); }
                return dragging;
            default: return false;
        }
    }
    @Override public boolean onTouchEvent(MotionEvent ev) {
        if (!isEnabled()) return isClickable() || isLongClickable();
        switch (ev.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: downX = ev.getX(); downY = lastY = ev.getY(); dragging = false; return true;
            case MotionEvent.ACTION_MOVE:
                if (!dragging && Math.abs(ev.getY() - downY) > touchSlop) { dragging = true; scrollStateChanged(OnScrollListener.SCROLL_STATE_TOUCH_SCROLL); }
                if (dragging) { int dy = (int) (lastY - ev.getY()); if (dy != 0) { scrollListBy(dy); lastY -= dy; } }
                return true;
            case MotionEvent.ACTION_UP:
                if (dragging) scrollStateChanged(OnScrollListener.SCROLL_STATE_IDLE);
                else {
                    int position = pointToPosition((int) ev.getX(), (int) ev.getY());
                    if (position != INVALID_POSITION && mAdapter != null && mAdapter.isEnabled(position))
                        performItemClick(getChildAt(position - firstPosition), position, mAdapter.getItemId(position));
                }
                dragging = false;
                return true;
            case MotionEvent.ACTION_CANCEL:
                if (dragging) scrollStateChanged(OnScrollListener.SCROLL_STATE_IDLE);
                dragging = false;
                return true;
            default: return true;
        }
    }
    private void scrollStateChanged(int state) { if (onScrollListener != null) onScrollListener.onScrollStateChanged(this, state); }
    public int pointToPosition(int x, int y) {
        for (int i = getChildCount() - 1; i >= 0; --i) {
            View c = getChildAt(i);
            if (c.getVisibility() == VISIBLE && x >= c.getLeft() && x < c.getRight() && y >= c.getTop() && y < c.getBottom()) return firstPosition + i;
        }
        return INVALID_POSITION;
    }
    public long pointToRowId(int x, int y) { int p = pointToPosition(x, y); return p >= 0 && mAdapter != null ? mAdapter.getItemId(p) : INVALID_ROW_ID; }

    @Override protected ViewGroup.LayoutParams generateDefaultLayoutParams() { return new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT, 0); }
    @Override protected ViewGroup.LayoutParams generateLayoutParams(ViewGroup.LayoutParams p) { return new LayoutParams(p); }
    @Override public LayoutParams generateLayoutParams(AttributeSet attrs) { return new LayoutParams(getContext(), attrs); }
    @Override protected boolean checkLayoutParams(ViewGroup.LayoutParams p) { return p instanceof LayoutParams; }

    @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
    @Override public void onTextChanged(CharSequence s, int start, int before, int count) {
        if (textFilter && mAdapter instanceof Filterable f) f.getFilter().filter(s, this);
    }
    @Override public void afterTextChanged(Editable s) {}
    @Override public void onFilterComplete(int count) {}
    @Override public void onGlobalLayout() {}
    @Override public void onTouchModeChanged(boolean isInTouchMode) {}
    public int getListPaddingTop() { return getPaddingTop(); }
    public int getListPaddingBottom() { return getPaddingBottom(); }
    public int getListPaddingLeft() { return getPaddingLeft(); }
    public int getListPaddingRight() { return getPaddingRight(); }
    private boolean selectedChildViewEnabled;
    public boolean isSelectedChildViewEnabled() { return selectedChildViewEnabled; }
    public void setSelectedChildViewEnabled(boolean selectedChildViewEnabled) { this.selectedChildViewEnabled = selectedChildViewEnabled; }
    /** lays the children out again: a layout pass does */
    protected void layoutChildren() { requestLayout(); }
    @Override public CharSequence getAccessibilityClassName() { return AbsListView.class.getName(); }
}
