// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.animation;

/** Animation helpers: the frame clock. */
public class AnimationUtils {
    public static long currentAnimationTimeMillis() { return android.os.SystemClock.uptimeMillis(); }
    public static Interpolator loadInterpolator(android.content.Context context, int id) { return new AccelerateDecelerateInterpolator(); }
}
