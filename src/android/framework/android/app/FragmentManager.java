// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import java.util.ArrayList;

/** An activity's platform fragments, which follow its lifecycle. */
@Deprecated
public class FragmentManager {
    private final Activity activity;
    private final ArrayList<Fragment> fragments = new ArrayList<>();
    private final ArrayList<FragmentTransaction> pending = new ArrayList<>();
    private int state; // 0 none, 1 created, 2 started, 3 resumed

    FragmentManager(Activity activity) { this.activity = activity; }
    public FragmentTransaction beginTransaction() { return new FragmentTransaction(this); }
    void enqueue(FragmentTransaction t) { pending.add(t); }
    public boolean executePendingTransactions() {
        boolean any = !pending.isEmpty();
        while (!pending.isEmpty()) {
            for (Fragment f : pending.remove(0).added) {
                fragments.add(f);
                f.attach(activity);
                if (state >= 1) f.onActivityCreated(null);
                if (state >= 2) f.onStart();
                if (state >= 3) f.onResume();
            }
        }
        return any;
    }
    public Fragment findFragmentByTag(String tag) {
        for (Fragment f : fragments) if (tag != null && tag.equals(f.tag)) return f;
        for (FragmentTransaction t : pending) for (Fragment f : t.added) if (tag != null && tag.equals(f.tag)) return f;
        return null;
    }
    public Fragment findFragmentById(int id) { return null; }
    public boolean popBackStackImmediate() { return false; }
    public void popBackStack() {}
    public int getBackStackEntryCount() { return 0; }
    public boolean isDestroyed() { return state < 0; }
    void dispatchCreate() { executePendingTransactions(); state = 1; for (Fragment f : list()) f.onActivityCreated(null); }
    void dispatchStart() { executePendingTransactions(); state = 2; for (Fragment f : list()) f.onStart(); }
    void dispatchResume() { executePendingTransactions(); state = 3; for (Fragment f : list()) f.onResume(); }
    void dispatchPause() { state = 2; for (Fragment f : list()) f.onPause(); }
    void dispatchStop() { state = 1; for (Fragment f : list()) f.onStop(); }
    void dispatchDestroy() { state = -1; for (Fragment f : list()) { f.onDestroy(); f.onDetach(); } }
    private Fragment[] list() { return fragments.toArray(new Fragment[0]); }
}
