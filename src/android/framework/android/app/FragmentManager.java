// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import java.util.ArrayList;

/** An activity's or a fragment's platform fragments, which follow its lifecycle. */
@Deprecated
public class FragmentManager {
    private final Activity activity;
    private final Fragment parent;
    private final ArrayList<Fragment> fragments = new ArrayList<>();
    private final ArrayList<FragmentTransaction> pending = new ArrayList<>();
    int state; // -1 destroyed, 0 none, 1 created, 2 started, 3 resumed

    FragmentManager(Activity activity) { this(activity, null); }
    FragmentManager(Activity activity, Fragment parent) { this.activity = activity; this.parent = parent; }
    public FragmentTransaction beginTransaction() { return new FragmentTransaction(this); }
    void enqueue(FragmentTransaction t) { pending.add(t); }
    public boolean executePendingTransactions() {
        boolean any = !pending.isEmpty();
        while (!pending.isEmpty()) {
            for (Fragment f : pending.remove(0).added) {
                fragments.add(f);
                f.parent = parent;
                f.attach(activity);
                if (state >= 1) f.performActivityCreated();
                if (state >= 2) f.performStart();
                if (state >= 3) f.performResume();
            }
        }
        return any;
    }
    /** Takes a fragment out: it goes down through its lifecycle to detached. */
    void remove(Fragment f) {
        if (!fragments.remove(f)) return;
        if (f.state >= 3) f.performPause();
        if (f.state >= 2) f.performStop();
        f.performDestroy();
    }
    public Fragment findFragmentByTag(String tag) {
        for (Fragment f : fragments) if (tag != null && tag.equals(f.tag)) return f;
        for (FragmentTransaction t : pending) for (Fragment f : t.added) if (tag != null && tag.equals(f.tag)) return f;
        return null;
    }
    public Fragment findFragmentById(int id) {
        for (Fragment f : fragments) if (id != 0 && f.id == id) return f;
        return null;
    }
    public void putFragment(android.os.Bundle bundle, String key, Fragment fragment) {
        int i = fragments.indexOf(fragment);
        if (i < 0) throw new IllegalStateException("Fragment " + fragment + " is not currently in the FragmentManager");
        bundle.putInt(key, i);
    }
    public Fragment getFragment(android.os.Bundle bundle, String key) {
        int i = bundle.getInt(key, -1);
        if (i == -1) return null;
        if (i >= fragments.size()) throw new IllegalStateException("Fragment no longer exists for key " + key + ": index " + i);
        return fragments.get(i);
    }
    public boolean popBackStackImmediate() { return false; }
    public void popBackStack() {}
    public int getBackStackEntryCount() { return 0; }
    public boolean isDestroyed() { return state < 0; }
    public void dump(String prefix, java.io.FileDescriptor fd, java.io.PrintWriter writer, String[] args) {
        for (Fragment f : list()) f.dump(prefix, fd, writer, args);
    }
    void dispatchCreate() { executePendingTransactions(); state = 1; for (Fragment f : list()) f.performActivityCreated(); }
    void dispatchStart() { executePendingTransactions(); state = 2; for (Fragment f : list()) f.performStart(); }
    void dispatchResume() { executePendingTransactions(); state = 3; for (Fragment f : list()) f.performResume(); }
    void dispatchPause() { state = 2; for (Fragment f : list()) f.performPause(); }
    void dispatchStop() { state = 1; for (Fragment f : list()) f.performStop(); }
    void dispatchDestroy() { state = -1; for (Fragment f : list()) f.performDestroy(); }
    private Fragment[] list() { return fragments.toArray(new Fragment[0]); }
}
