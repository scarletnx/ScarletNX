// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

/** Shared element transitions: none run here. */
public abstract class SharedElementCallback {
    public interface OnSharedElementsReadyListener { void onSharedElementsReady(); }
    public void onSharedElementsArrived(java.util.List<String> sharedElementNames, java.util.List<android.view.View> sharedElements, OnSharedElementsReadyListener listener) {
        listener.onSharedElementsReady();
    }
}
