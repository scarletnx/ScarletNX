// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.app;

import android.graphics.Rect;
import android.os.Bundle;
import android.util.Pair;
import android.view.View;

/** How to start an activity: the bounds and the display are kept, the animations have no effect on the one window. */
public class ActivityOptions {
    public static final int MODE_BACKGROUND_ACTIVITY_START_SYSTEM_DEFINED = 0, MODE_BACKGROUND_ACTIVITY_START_ALLOWED = 1,
        MODE_BACKGROUND_ACTIVITY_START_DENIED = 2, MODE_BACKGROUND_ACTIVITY_START_ALLOW_ALWAYS = 3;
    private static final String KEY_LAUNCH_BOUNDS = "android:activity.launchBounds", KEY_LAUNCH_DISPLAY_ID = "android.activity.launchDisplayId",
        KEY_PENDING_INTENT_BACKGROUND_ACTIVITY_START_MODE = "android.pendingIntent.backgroundActivityAllowed";
    private Rect launchBounds;
    private int launchDisplayId = -1, backgroundStartMode = MODE_BACKGROUND_ACTIVITY_START_SYSTEM_DEFINED;

    private ActivityOptions() {}
    public static ActivityOptions makeBasic() { return new ActivityOptions(); }
    public static ActivityOptions makeCustomAnimation(android.content.Context context, int enterResId, int exitResId) { return new ActivityOptions(); }
    public static ActivityOptions makeScaleUpAnimation(View source, int startX, int startY, int width, int height) { return new ActivityOptions(); }
    public static ActivityOptions makeClipRevealAnimation(View source, int startX, int startY, int width, int height) { return new ActivityOptions(); }
    public static ActivityOptions makeThumbnailScaleUpAnimation(View source, android.graphics.Bitmap thumbnail, int startX, int startY) { return new ActivityOptions(); }
    public static ActivityOptions makeSceneTransitionAnimation(Activity activity, View sharedElement, String sharedElementName) { return new ActivityOptions(); }
    @SafeVarargs
    public static ActivityOptions makeSceneTransitionAnimation(Activity activity, Pair<View, String>... sharedElements) { return new ActivityOptions(); }
    public static ActivityOptions makeTaskLaunchBehind() { return new ActivityOptions(); }

    public Rect getLaunchBounds() { return launchBounds; }
    public ActivityOptions setLaunchBounds(Rect screenSpacePixelRect) { launchBounds = screenSpacePixelRect != null ? new Rect(screenSpacePixelRect) : null; return this; }
    public int getLaunchDisplayId() { return launchDisplayId; }
    public ActivityOptions setLaunchDisplayId(int launchDisplayId) { this.launchDisplayId = launchDisplayId; return this; }
    public ActivityOptions setPendingIntentBackgroundActivityStartMode(int state) { backgroundStartMode = state; return this; }
    public int getPendingIntentBackgroundActivityStartMode() { return backgroundStartMode; }
    /** No usage time is measured, so the report never comes. */
    public void requestUsageTimeReport(PendingIntent receiver) {}
    public void update(ActivityOptions otherOptions) {
        if (otherOptions.launchBounds != null) launchBounds = otherOptions.launchBounds;
        if (otherOptions.launchDisplayId != -1) launchDisplayId = otherOptions.launchDisplayId;
        if (otherOptions.backgroundStartMode != MODE_BACKGROUND_ACTIVITY_START_SYSTEM_DEFINED) backgroundStartMode = otherOptions.backgroundStartMode;
    }
    public Bundle toBundle() {
        Bundle b = new Bundle();
        if (launchBounds != null) b.putParcelable(KEY_LAUNCH_BOUNDS, launchBounds);
        if (launchDisplayId != -1) b.putInt(KEY_LAUNCH_DISPLAY_ID, launchDisplayId);
        if (backgroundStartMode != MODE_BACKGROUND_ACTIVITY_START_SYSTEM_DEFINED) b.putInt(KEY_PENDING_INTENT_BACKGROUND_ACTIVITY_START_MODE, backgroundStartMode);
        return b;
    }
    @Override public String toString() { return "ActivityOptions(" + hashCode() + "), launchBounds=" + launchBounds + ", launchDisplayId=" + launchDisplayId; }
}
