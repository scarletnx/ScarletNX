// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.content.Context;
import android.os.Bundle;

/** The platform fragment (deprecated): only its lifecycle, in its activity's. */
@Deprecated
public class Fragment {
    Activity activity;
    String tag;
    private boolean added;

    public Fragment() {}
    public final Activity getActivity() { return activity; }
    public Context getContext() { return activity; }
    public final String getTag() { return tag; }
    public final boolean isAdded() { return added; }
    public final boolean isDetached() { return !added; }
    public final boolean isRemoving() { return false; }
    public final FragmentManager getFragmentManager() { return activity != null ? activity.getFragmentManager() : null; }
    public void onAttach(Context context) {}
    @Deprecated public void onAttach(Activity activity) {}
    public void onCreate(Bundle savedInstanceState) {}
    public void onActivityCreated(Bundle savedInstanceState) {}
    public void onStart() {}
    public void onResume() {}
    public void onPause() {}
    public void onStop() {}
    public void onDestroy() {}
    public void onDetach() {}
    public void setRetainInstance(boolean retain) {}
    void attach(Activity a) {
        activity = a;
        added = true;
        onAttach((Context) a);
        onAttach(a);
        onCreate(null);
    }
}
