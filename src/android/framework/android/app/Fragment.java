// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2010 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.app;

import android.content.ComponentCallbacks2;
import android.content.Context;
import android.content.Intent;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.os.Bundle;
import android.view.ContextMenu;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;

/** The platform fragment (deprecated): its lifecycle, in its activity's, and a view that it does not place. */
@Deprecated
public class Fragment implements ComponentCallbacks2, View.OnCreateContextMenuListener {
    Activity activity;
    Fragment parent;
    String tag;
    int id, state; // state: 0 attached, 1 activity created, 2 started, 3 resumed
    private boolean added, retain, userVisibleHint = true;
    private Bundle arguments;
    private View view;
    private Fragment target;
    private int targetRequestCode;
    private FragmentManager children;

    public Fragment() {}
    public final Activity getActivity() { return activity; }
    public Context getContext() { return activity; }
    private Activity host() {
        if (activity == null) throw new IllegalStateException("Fragment " + this + " not attached to Activity");
        return activity;
    }
    public final Resources getResources() { return host().getResources(); }
    public final CharSequence getText(int resId) { return getResources().getText(resId); }
    public final String getString(int resId) { return getResources().getString(resId); }
    public final String getTag() { return tag; }
    public final int getId() { return id; }
    public final boolean isAdded() { return added; }
    public final boolean isDetached() { return !added; }
    public final boolean isRemoving() { return false; }
    public final boolean isResumed() { return state == 3; }
    public final boolean isHidden() { return false; }
    public final boolean isInLayout() { return false; }
    public final boolean isVisible() {
        return added && view != null && view.isAttachedToWindow() && view.getVisibility() == View.VISIBLE;
    }
    public final FragmentManager getFragmentManager() { return parent != null ? parent.getChildFragmentManager() : activity != null ? activity.getFragmentManager() : null; }
    public final FragmentManager getChildFragmentManager() {
        if (children == null) {
            if (activity == null) throw new IllegalStateException("Fragment " + this + " has not been attached yet.");
            children = new FragmentManager(activity, this);
            children.state = state;
        }
        return children;
    }
    public final Fragment getParentFragment() { return parent; }
    public void setArguments(Bundle args) { arguments = args; }
    public final Bundle getArguments() { return arguments; }
    public void setRetainInstance(boolean retain) { this.retain = retain; }
    public final boolean getRetainInstance() { return retain; }
    public void setTargetFragment(Fragment fragment, int requestCode) { target = fragment; targetRequestCode = requestCode; }
    public final Fragment getTargetFragment() { return target; }
    public final int getTargetRequestCode() { return targetRequestCode; }
    public void setUserVisibleHint(boolean isVisibleToUser) { userVisibleHint = isVisibleToUser; }
    public boolean getUserVisibleHint() { return userVisibleHint; }
    public void setHasOptionsMenu(boolean hasMenu) {}
    public void setMenuVisibility(boolean menuVisible) {}
    public View getView() { return view; }

    public void onAttach(Context context) {}
    @Deprecated public void onAttach(Activity activity) {}
    public void onCreate(Bundle savedInstanceState) {}
    public View onCreateView(LayoutInflater inflater, ViewGroup container, Bundle savedInstanceState) { return null; }
    public void onViewCreated(View view, Bundle savedInstanceState) {}
    public void onActivityCreated(Bundle savedInstanceState) {}
    public void onStart() {}
    public void onResume() {}
    public void onPause() {}
    public void onStop() {}
    public void onDestroyView() {}
    public void onDestroy() {}
    public void onDetach() {}
    public void onSaveInstanceState(Bundle outState) {}
    public void onHiddenChanged(boolean hidden) {}
    @Override public void onConfigurationChanged(Configuration newConfig) {}
    @Override public void onLowMemory() {}
    @Override public void onTrimMemory(int level) {}
    /** Results go to the activity's onActivityResult(), not to the fragment's. */
    public void onActivityResult(int requestCode, int resultCode, Intent data) {}
    public void startActivity(Intent intent) { host().startActivity(intent); }
    public void startActivity(Intent intent, Bundle options) { host().startActivity(intent, options); }
    public void startActivityForResult(Intent intent, int requestCode) { host().startActivityForResult(intent, requestCode); }
    public void startActivityForResult(Intent intent, int requestCode, Bundle options) { host().startActivityForResult(intent, requestCode, options); }
    public void registerForContextMenu(View view) { view.setOnCreateContextMenuListener(this); }
    public void unregisterForContextMenu(View view) { view.setOnCreateContextMenuListener(null); }
    @Override public void onCreateContextMenu(ContextMenu menu, View v, ContextMenu.ContextMenuInfo menuInfo) { host().onCreateContextMenu(menu, v, menuInfo); }
    public void dump(String prefix, java.io.FileDescriptor fd, java.io.PrintWriter writer, String[] args) {
        writer.print(prefix);
        writer.println(getClass().getName() + " id=" + id + " tag=" + tag + " state=" + state + " added=" + added + " view=" + view);
        if (children != null) children.dump(prefix + "  ", fd, writer, args);
    }

    void attach(Activity a) {
        activity = a;
        added = true;
        onAttach((Context) a);
        onAttach(a);
        onCreate(null);
        view = onCreateView(a.getLayoutInflater(), null, null);
        if (view != null) onViewCreated(view, null);
    }
    void performActivityCreated() { state = 1; onActivityCreated(null); if (children != null) children.dispatchCreate(); }
    void performStart() { state = 2; onStart(); if (children != null) children.dispatchStart(); }
    void performResume() { state = 3; onResume(); if (children != null) children.dispatchResume(); }
    void performPause() { if (children != null) children.dispatchPause(); state = 2; onPause(); }
    void performStop() { if (children != null) children.dispatchStop(); state = 1; onStop(); }
    void performDestroy() {
        if (children != null) children.dispatchDestroy();
        if (view != null) { onDestroyView(); view = null; }
        onDestroy();
        added = false;
        onDetach();
    }
}
