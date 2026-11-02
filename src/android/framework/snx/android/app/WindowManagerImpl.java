// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package snx.android.app;

import android.view.Display;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewRootImpl;
import android.view.WindowManager;
import java.util.IdentityHashMap;

/** The window manager: each added view gets its own root, and the host shows the roots on its one window. */
final class WindowManagerImpl implements WindowManager {
    private static final IdentityHashMap<View, ViewRootImpl> roots = new IdentityHashMap<>();
    private final android.content.Context context;

    WindowManagerImpl(android.content.Context context) { this.context = context; }

    @Override @Deprecated public Display getDefaultDisplay() { return Display.DEFAULT; }
    @Override public void addView(View view, ViewGroup.LayoutParams params) {
        if (!(params instanceof WindowManager.LayoutParams lp)) throw new IllegalArgumentException("Params must be WindowManager.LayoutParams");
        synchronized (roots) {
            if (roots.containsKey(view)) throw new IllegalStateException("View " + view + " has already been added to the window manager.");
            view.setLayoutParams(lp);
            ViewRootImpl root = new ViewRootImpl(view.getContext() != null ? view.getContext() : context);
            roots.put(view, root);
            root.setView(view, lp, null);
        }
    }
    @Override public void updateViewLayout(View view, ViewGroup.LayoutParams params) {
        view.setLayoutParams(params);
        view.requestLayout();
    }
    @Override public void removeView(View view) { removeViewImmediate(view); }
    @Override public void removeViewImmediate(View view) {
        ViewRootImpl root;
        synchronized (roots) { root = roots.remove(view); }
        if (root == null) throw new IllegalArgumentException("View=" + view + " not attached to window manager");
        root.die();
    }
}
