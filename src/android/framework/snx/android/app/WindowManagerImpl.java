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

    /** the display its windows go to, its context's (asked when needed: an activity's has no base when this is made):
        the host window's, or a virtual one's, which nothing composes */
    private Display display() {
        Display d = null;
        try {
            d = context != null ? context.getDisplay() : null;
        } catch (RuntimeException e) {
            // a context not attached yet
        }
        return d != null ? d : Display.DEFAULT;
    }

    @Override @Deprecated public Display getDefaultDisplay() { return display(); }
    @Override public void addView(View view, ViewGroup.LayoutParams params) {
        if (!(params instanceof WindowManager.LayoutParams lp)) throw new IllegalArgumentException("Params must be WindowManager.LayoutParams");
        synchronized (roots) {
            if (roots.containsKey(view)) throw new IllegalStateException("View " + view + " has already been added to the window manager.");
            view.setLayoutParams(lp);
            Display display = display();
            if (!display.isValid()) throw new WindowManager.InvalidDisplayException("Unable to add window: the display " + display + " was removed");
            ViewRootImpl root = new ViewRootImpl(view.getContext() != null ? view.getContext() : context);
            root.setDisplay(display);
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
