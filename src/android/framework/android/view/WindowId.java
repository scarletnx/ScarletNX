// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** A window's identity, as View.getWindowId gives it: one for each window's root view. */
public class WindowId implements android.os.Parcelable {
    private static final java.util.WeakHashMap<View, WindowId> ids = new java.util.WeakHashMap<>();
    private final java.lang.ref.WeakReference<View> root;

    private WindowId(View root) { this.root = new java.lang.ref.WeakReference<>(root); }
    static synchronized WindowId of(View root) { return ids.computeIfAbsent(root, WindowId::new); }

    public boolean isFocused() {
        View r = root.get();
        return r != null && r.hasWindowFocus();
    }
    public abstract static class FocusObserver {
        public abstract void onFocusGained(WindowId token);
        public abstract void onFocusLost(WindowId token);
    }
    public void registerFocusObserver(FocusObserver observer) {}
    public void unregisterFocusObserver(FocusObserver observer) {}
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) {}
}
