// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.window;

/** Told of a back gesture as it goes. */
public interface OnBackAnimationCallback extends OnBackInvokedCallback {
    default void onBackStarted(BackEvent backEvent) {}
    default void onBackProgressed(BackEvent backEvent) {}
    default void onBackCancelled() {}
}
