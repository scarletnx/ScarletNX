// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.window;

import java.util.ArrayList;

/** The callbacks of the back key, by priority: the last registered of the highest priority is called. */
public interface OnBackInvokedDispatcher {
    int PRIORITY_DEFAULT = 0, PRIORITY_OVERLAY = 1000000, PRIORITY_SYSTEM_NAVIGATION_OBSERVER = -2;

    void registerOnBackInvokedCallback(int priority, OnBackInvokedCallback callback);
    void unregisterOnBackInvokedCallback(OnBackInvokedCallback callback);

    /** A window's dispatcher; its fallback goes to the activity when no callback is registered. */
    final class Default implements OnBackInvokedDispatcher {
        private final ArrayList<Object[]> callbacks = new ArrayList<>();
        private Runnable fallback;

        public void setFallback(Runnable fallback) { this.fallback = fallback; }
        @Override public synchronized void registerOnBackInvokedCallback(int priority, OnBackInvokedCallback callback) {
            unregisterOnBackInvokedCallback(callback);
            callbacks.add(new Object[] {priority, callback});
        }
        @Override public synchronized void unregisterOnBackInvokedCallback(OnBackInvokedCallback callback) {
            callbacks.removeIf(c -> c[1] == callback);
        }
        /** The back key: the top callback, or the fallback. True when something handled it. */
        public boolean invoke() {
            OnBackInvokedCallback top = null;
            int best = Integer.MIN_VALUE;
            synchronized (this) {
                for (Object[] c : callbacks) if ((Integer) c[0] >= best && (Integer) c[0] >= 0) { best = (Integer) c[0]; top = (OnBackInvokedCallback) c[1]; }
            }
            if (top != null) { top.onBackInvoked(); return true; }
            if (fallback != null) { fallback.run(); return true; }
            return false;
        }
    }
}
