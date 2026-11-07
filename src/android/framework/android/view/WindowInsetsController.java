// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

/** Shows and hides the system bars and the keyboard: here, a record of the requests. */
public interface WindowInsetsController {
    int APPEARANCE_LIGHT_STATUS_BARS = 8, APPEARANCE_LIGHT_NAVIGATION_BARS = 16, APPEARANCE_TRANSPARENT_CAPTION_BAR_BACKGROUND = 256;
    int BEHAVIOR_DEFAULT = 1, BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE = 2;

    interface OnControllableInsetsChangedListener {
        void onControllableInsetsChanged(WindowInsetsController controller, int typeMask);
    }

    void show(int types);
    void hide(int types);
    void setSystemBarsAppearance(int appearance, int mask);
    int getSystemBarsAppearance();
    void setSystemBarsBehavior(int behavior);
    int getSystemBarsBehavior();
    void addOnControllableInsetsChangedListener(OnControllableInsetsChangedListener listener);
    void removeOnControllableInsetsChangedListener(OnControllableInsetsChangedListener listener);
    default void controlWindowInsetsAnimation(int types, long durationMillis, android.view.animation.Interpolator interpolator,
                                              android.os.CancellationSignal cancellationSignal, WindowInsetsAnimationControlListener listener) {}

    /** The controller of a window. */
    final class Simple implements WindowInsetsController {
        private int appearance, behavior = BEHAVIOR_DEFAULT;
        @Override public void show(int types) {}
        @Override public void hide(int types) {}
        @Override public void setSystemBarsAppearance(int appearance, int mask) { this.appearance = (this.appearance & ~mask) | (appearance & mask); }
        @Override public int getSystemBarsAppearance() { return appearance; }
        @Override public void setSystemBarsBehavior(int behavior) { this.behavior = behavior; }
        @Override public int getSystemBarsBehavior() { return behavior; }
        @Override public void addOnControllableInsetsChangedListener(OnControllableInsetsChangedListener listener) { listener.onControllableInsetsChanged(this, 0); }
        @Override public void removeOnControllableInsetsChangedListener(OnControllableInsetsChangedListener listener) {}
    }
}
