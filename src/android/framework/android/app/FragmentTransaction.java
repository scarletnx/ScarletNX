// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import java.util.ArrayList;

/** Fragments to add or remove. */
@Deprecated
public class FragmentTransaction {
    private final FragmentManager manager;
    final ArrayList<Fragment> added = new ArrayList<>();

    FragmentTransaction(FragmentManager manager) { this.manager = manager; }
    public FragmentTransaction add(Fragment fragment, String tag) { fragment.tag = tag; added.add(fragment); return this; }
    public FragmentTransaction add(int containerViewId, Fragment fragment) { return add(fragment, null); }
    public FragmentTransaction add(int containerViewId, Fragment fragment, String tag) { return add(fragment, tag); }
    public FragmentTransaction remove(Fragment fragment) { return this; }
    public FragmentTransaction replace(int containerViewId, Fragment fragment) { return add(fragment, null); }
    public FragmentTransaction addToBackStack(String name) { return this; }
    public int commit() { manager.enqueue(this); return 0; }
    public int commitAllowingStateLoss() { return commit(); }
    public void commitNow() { commit(); manager.executePendingTransactions(); }
    public void commitNowAllowingStateLoss() { commitNow(); }
}
