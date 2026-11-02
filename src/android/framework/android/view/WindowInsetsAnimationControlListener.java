// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** Told of an insets animation the app controls. */
public interface WindowInsetsAnimationControlListener {
    void onReady(WindowInsetsAnimationController controller, int types);
    void onFinished(WindowInsetsAnimationController controller);
    void onCancelled(WindowInsetsAnimationController controller);
}
