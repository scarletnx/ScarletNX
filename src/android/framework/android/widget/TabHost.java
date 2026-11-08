// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2006 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.widget;

import android.content.Context;
import android.content.Intent;
import android.graphics.drawable.Drawable;
import android.os.Parcel;
import android.os.Parcelable;
import android.util.AttributeSet;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import java.util.ArrayList;

/**
 * Tabs: a TabWidget of indicators, with the id android.R.id.tabs, and a FrameLayout of the content of each tab, with
 * the id android.R.id.tabcontent. The content of the current tab shows; the others are gone.
 */
public class TabHost extends FrameLayout implements ViewTreeObserver.OnTouchModeChangeListener {
    private static final int ID_TABS = 0x01020013, ID_TABCONTENT = 0x01020011; // android.R.id.tabs and tabcontent
    private TabWidget tabWidget;
    private FrameLayout tabContent;
    private final ArrayList<TabSpec> tabSpecs = new ArrayList<>(2);
    private int currentTab = -1;
    private View currentView;
    private OnTabChangeListener onTabChangeListener;

    public interface OnTabChangeListener { void onTabChanged(String tabId); }
    public interface TabContentFactory { View createTabContent(String tag); }

    public TabHost(Context context) { super(context); }
    public TabHost(Context context, AttributeSet attrs) { super(context, attrs); }
    public TabHost(Context context, AttributeSet attrs, int defStyleAttr) { super(context, attrs, defStyleAttr); }
    public TabHost(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) { super(context, attrs, defStyleAttr, defStyleRes); }

    /** A tab: its tag, its indicator in the TabWidget, and its content. */
    public class TabSpec {
        private final String tag;
        private View indicator;
        private ContentStrategy content;

        private TabSpec(String tag) { this.tag = tag; }
        public TabSpec setIndicator(CharSequence label) { return setIndicator(label, null); }
        public TabSpec setIndicator(CharSequence label, Drawable icon) {
            TextView t = new TextView(getContext());
            t.setText(label);
            t.setGravity(android.view.Gravity.CENTER);
            if (icon != null) t.setCompoundDrawablesWithIntrinsicBounds(null, icon, null, null);
            t.setLayoutParams(new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
            indicator = t;
            return this;
        }
        public TabSpec setIndicator(View view) { indicator = view; return this; }
        public TabSpec setContent(int viewId) {
            View v = tabContent.findViewById(viewId);
            if (v == null) throw new RuntimeException("Could not create tab content because could not find view with id " + viewId);
            v.setVisibility(GONE);
            content = new ContentStrategy() {
                @Override public View getContentView() { v.setVisibility(VISIBLE); return v; }
                @Override public void tabClosed() { v.setVisibility(GONE); }
            };
            return this;
        }
        public TabSpec setContent(TabContentFactory contentFactory) {
            content = new ContentStrategy() {
                private View view;
                @Override public View getContentView() {
                    if (view == null) view = contentFactory.createTabContent(tag);
                    view.setVisibility(VISIBLE);
                    return view;
                }
                @Override public void tabClosed() { if (view != null) view.setVisibility(GONE); }
            };
            return this;
        }
        /** An activity in a tab needs a LocalActivityManager, which the runtime does not have: the tab shows nothing. */
        public TabSpec setContent(Intent intent) {
            View empty = new View(getContext());
            content = new ContentStrategy() {
                @Override public View getContentView() { empty.setVisibility(VISIBLE); return empty; }
                @Override public void tabClosed() { empty.setVisibility(GONE); }
            };
            return this;
        }
        public String getTag() { return tag; }
    }
    private interface ContentStrategy { View getContentView(); void tabClosed(); }

    public TabSpec newTabSpec(String tag) {
        if (tag == null) throw new IllegalArgumentException("tag must be non-null");
        return new TabSpec(tag);
    }
    public void setup() {
        tabWidget = findViewById(ID_TABS);
        if (tabWidget == null) throw new RuntimeException("Your TabHost must have a TabWidget whose id attribute is 'android.R.id.tabs'");
        tabWidget.selectionChangedListener = (tabIndex, clicked) -> setCurrentTab(tabIndex);
        tabContent = findViewById(ID_TABCONTENT);
        if (tabContent == null) throw new RuntimeException("Your TabHost must have a FrameLayout whose id attribute is 'android.R.id.tabcontent'");
    }
    public void addTab(TabSpec tabSpec) {
        if (tabSpec.indicator == null) throw new IllegalArgumentException("you must specify a way to create the tab indicator.");
        if (tabSpec.content == null) throw new IllegalArgumentException("you must specify a way to create the tab content");
        tabWidget.addView(tabSpec.indicator);
        tabSpecs.add(tabSpec);
        if (currentTab == -1) setCurrentTab(0);
    }
    public void clearAllTabs() {
        tabWidget.removeAllViews();
        tabContent.removeAllViews();
        tabSpecs.clear();
        currentTab = -1;
        currentView = null;
        requestLayout();
        invalidate();
    }
    public TabWidget getTabWidget() { return tabWidget; }
    public int getCurrentTab() { return currentTab; }
    public String getCurrentTabTag() { return currentTab >= 0 && currentTab < tabSpecs.size() ? tabSpecs.get(currentTab).tag : null; }
    public View getCurrentTabView() { return currentTab >= 0 && currentTab < tabSpecs.size() ? tabWidget.getChildTabViewAt(currentTab) : null; }
    public View getCurrentView() { return currentView; }
    public FrameLayout getTabContentView() { return tabContent; }
    public void setCurrentTabByTag(String tag) {
        for (int i = 0; i < tabSpecs.size(); ++i) if (tabSpecs.get(i).tag.equals(tag)) { setCurrentTab(i); return; }
    }
    public void setCurrentTab(int index) {
        if (index < 0 || index >= tabSpecs.size() || index == currentTab) return;
        if (currentTab != -1) tabSpecs.get(currentTab).content.tabClosed();
        currentTab = index;
        TabSpec spec = tabSpecs.get(index);
        tabWidget.focusCurrentTab(currentTab);
        currentView = spec.content.getContentView();
        if (currentView.getParent() == null) tabContent.addView(currentView, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        if (!tabWidget.hasFocus()) currentView.requestFocus();
        if (onTabChangeListener != null) onTabChangeListener.onTabChanged(getCurrentTabTag());
    }
    public void setOnTabChangedListener(OnTabChangeListener l) { onTabChangeListener = l; }

    @Override protected void onAttachedToWindow() { super.onAttachedToWindow(); getViewTreeObserver().addOnTouchModeChangeListener(this); }
    @Override protected void onDetachedFromWindow() { super.onDetachedFromWindow(); getViewTreeObserver().removeOnTouchModeChangeListener(this); }
    @Override public void onTouchModeChanged(boolean isInTouchMode) {}

    static class SavedState extends BaseSavedState {
        final String currentTab;
        SavedState(Parcelable superState, String currentTab) { super(superState); this.currentTab = currentTab; }
        SavedState(Parcel in) { super(in); currentTab = in.readString(); }
        @Override public void writeToParcel(Parcel out, int flags) { super.writeToParcel(out, flags); out.writeString(currentTab); }
        public static final Parcelable.Creator<SavedState> CREATOR = new Parcelable.Creator<SavedState>() {
            @Override public SavedState createFromParcel(Parcel in) { return new SavedState(in); }
            @Override public SavedState[] newArray(int size) { return new SavedState[size]; }
        };
    }
    @Override protected Parcelable onSaveInstanceState() { return new SavedState(super.onSaveInstanceState(), getCurrentTabTag()); }
    @Override protected void onRestoreInstanceState(Parcelable state) {
        if (!(state instanceof SavedState s)) { super.onRestoreInstanceState(state); return; }
        super.onRestoreInstanceState(s.getSuperState());
        if (s.currentTab != null) setCurrentTabByTag(s.currentTab);
    }
    @Override public CharSequence getAccessibilityClassName() { return TabHost.class.getName(); }
}
