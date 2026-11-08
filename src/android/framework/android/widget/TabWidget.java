// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.widget;

import android.content.Context;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;
import android.view.View;

/** The row of tab indicators of a TabHost. A click on an indicator selects its tab. */
public class TabWidget extends LinearLayout {
    private int selectedTab = -1;
    private boolean stripEnabled = true;
    private Drawable leftStrip, rightStrip;
    OnTabSelectionChanged selectionChangedListener;

    /** Tells the TabHost which tab the user clicked. */
    interface OnTabSelectionChanged { void onTabSelectionChanged(int tabIndex, boolean clicked); }

    public TabWidget(Context context) { this(context, null); }
    public TabWidget(Context context, AttributeSet attrs) { this(context, attrs, 0); }
    public TabWidget(Context context, AttributeSet attrs, int defStyleAttr) { this(context, attrs, defStyleAttr, 0); }
    public TabWidget(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
        setOrientation(HORIZONTAL);
    }

    public View getChildTabViewAt(int index) { return getChildAt(index); }
    public int getTabCount() { return getChildCount(); }
    @Override public void setDividerDrawable(Drawable drawable) { super.setDividerDrawable(drawable); }
    public void setDividerDrawable(int resId) { setDividerDrawable(getContext().getDrawable(resId)); }
    public void setLeftStripDrawable(Drawable drawable) { leftStrip = drawable; invalidate(); }
    public void setLeftStripDrawable(int resId) { setLeftStripDrawable(getContext().getDrawable(resId)); }
    public Drawable getLeftStripDrawable() { return leftStrip; }
    public void setRightStripDrawable(Drawable drawable) { rightStrip = drawable; invalidate(); }
    public void setRightStripDrawable(int resId) { setRightStripDrawable(getContext().getDrawable(resId)); }
    public Drawable getRightStripDrawable() { return rightStrip; }
    public void setStripEnabled(boolean stripEnabled) { this.stripEnabled = stripEnabled; invalidate(); }
    public boolean isStripEnabled() { return stripEnabled; }
    public void setCurrentTab(int index) {
        if (index < 0 || index >= getTabCount() || index == selectedTab) return;
        if (selectedTab >= 0 && selectedTab < getTabCount()) getChildTabViewAt(selectedTab).setSelected(false);
        selectedTab = index;
        getChildTabViewAt(index).setSelected(true);
    }
    public void focusCurrentTab(int index) {
        setCurrentTab(index);
        View v = getChildTabViewAt(index);
        if (v != null) v.requestFocus();
    }
    @Override public void setEnabled(boolean enabled) {
        super.setEnabled(enabled);
        for (int i = 0; i < getTabCount(); ++i) getChildTabViewAt(i).setEnabled(enabled);
    }
    @Override protected void onViewAdded(View child) {
        super.onViewAdded(child);
        child.setFocusable(true);
        child.setClickable(true);
        child.setOnClickListener(v -> { if (selectionChangedListener != null) selectionChangedListener.onTabSelectionChanged(indexOfChild(v), true); });
    }
    @Override public void removeAllViews() { super.removeAllViews(); selectedTab = -1; }
    @Override public CharSequence getAccessibilityClassName() { return TabWidget.class.getName(); }
}
