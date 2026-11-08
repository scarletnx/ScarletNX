// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view;

import android.graphics.Rect;
import android.os.IBinder;

/** Adds the windows of the process. Each window is a ViewRootImpl over the runtime's host window. */
public interface WindowManager extends ViewManager {
    String PROPERTY_ACTIVITY_EMBEDDING_SPLITS_ENABLED = "android.window.PROPERTY_ACTIVITY_EMBEDDING_SPLITS_ENABLED";

    @Deprecated Display getDefaultDisplay();
    void removeViewImmediate(View view);
    default WindowMetrics getCurrentWindowMetrics() { return WindowMetrics.current(); }
    default WindowMetrics getMaximumWindowMetrics() { return WindowMetrics.current(); }
    default boolean isCrossWindowBlurEnabled() { return false; }

    class InvalidDisplayException extends RuntimeException {
        public InvalidDisplayException() {}
        public InvalidDisplayException(String name) { super(name); }
    }
    class BadTokenException extends RuntimeException {
        public BadTokenException() {}
        public BadTokenException(String name) { super(name); }
    }

    class LayoutParams extends ViewGroup.LayoutParams implements android.os.Parcelable {
        public static final int TYPE_BASE_APPLICATION = 1, TYPE_APPLICATION = 2, TYPE_APPLICATION_STARTING = 3,
            TYPE_DRAWN_APPLICATION = 4, FIRST_SUB_WINDOW = 1000, TYPE_APPLICATION_PANEL = 1000, TYPE_APPLICATION_MEDIA = 1001,
            TYPE_APPLICATION_SUB_PANEL = 1002, TYPE_APPLICATION_ATTACHED_DIALOG = 1003, FIRST_SYSTEM_WINDOW = 2000,
            TYPE_TOAST = 2005, TYPE_APPLICATION_OVERLAY = 2038;
        public static final int FLAG_ALLOW_LOCK_WHILE_SCREEN_ON = 0x1, FLAG_DIM_BEHIND = 0x2, FLAG_BLUR_BEHIND = 0x4,
            FLAG_NOT_FOCUSABLE = 0x8, FLAG_NOT_TOUCHABLE = 0x10, FLAG_NOT_TOUCH_MODAL = 0x20, FLAG_KEEP_SCREEN_ON = 0x80,
            FLAG_LAYOUT_IN_SCREEN = 0x100, FLAG_LAYOUT_NO_LIMITS = 0x200, FLAG_FULLSCREEN = 0x400,
            FLAG_FORCE_NOT_FULLSCREEN = 0x800, FLAG_SECURE = 0x2000, FLAG_SCALED = 0x4000, FLAG_WATCH_OUTSIDE_TOUCH = 0x40000,
            FLAG_SHOW_WHEN_LOCKED = 0x80000, FLAG_HARDWARE_ACCELERATED = 0x01000000, FLAG_LAYOUT_IN_OVERSCAN = 0x02000000,
            FLAG_TRANSLUCENT_STATUS = 0x04000000, FLAG_TRANSLUCENT_NAVIGATION = 0x08000000,
            FLAG_LAYOUT_ATTACHED_IN_DECOR = 0x40000000, FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS = 0x80000000,
            FLAG_ALT_FOCUSABLE_IM = 0x20000;
        public static final int SOFT_INPUT_STATE_UNSPECIFIED = 0, SOFT_INPUT_STATE_UNCHANGED = 1, SOFT_INPUT_STATE_HIDDEN = 2,
            SOFT_INPUT_STATE_ALWAYS_HIDDEN = 3, SOFT_INPUT_STATE_VISIBLE = 4, SOFT_INPUT_STATE_ALWAYS_VISIBLE = 5,
            SOFT_INPUT_MASK_STATE = 0x0f, SOFT_INPUT_ADJUST_UNSPECIFIED = 0x00, SOFT_INPUT_ADJUST_RESIZE = 0x10,
            SOFT_INPUT_ADJUST_PAN = 0x20, SOFT_INPUT_ADJUST_NOTHING = 0x30, SOFT_INPUT_MASK_ADJUST = 0xf0,
            SOFT_INPUT_IS_FORWARD_NAVIGATION = 0x100;
        public static final int LAYOUT_IN_DISPLAY_CUTOUT_MODE_DEFAULT = 0, LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES = 1,
            LAYOUT_IN_DISPLAY_CUTOUT_MODE_NEVER = 2, LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS = 3;
        public static final int ROTATION_ANIMATION_ROTATE = 0;
        public static final float BRIGHTNESS_OVERRIDE_NONE = -1.0f;
        public int x, y, type, flags, privateFlags, softInputMode, gravity, format, windowAnimations, memoryType,
            systemUiVisibility, layoutInDisplayCutoutMode, rotationAnimation, screenOrientation = -1, preferredDisplayModeId;
        public float horizontalMargin, verticalMargin, alpha = 1, dimAmount = 1, screenBrightness = BRIGHTNESS_OVERRIDE_NONE,
            buttonBrightness = BRIGHTNESS_OVERRIDE_NONE, preferredRefreshRate;
        public IBinder token;
        public String packageName;
        private CharSequence title = "";
        private int fitInsetsTypes = WindowInsets.Type.systemBars(), fitInsetsSides = WindowInsets.Side.all();
        private boolean fitInsetsIgnoringVisibility;

        public LayoutParams() { super(MATCH_PARENT, MATCH_PARENT); type = TYPE_APPLICATION; format = android.graphics.PixelFormat.OPAQUE; }
        public LayoutParams(int type) { this(); this.type = type; }
        public LayoutParams(int type, int flags) { this(type); this.flags = flags; }
        public LayoutParams(int type, int flags, int format) { this(type, flags); this.format = format; }
        public LayoutParams(int w, int h, int type, int flags, int format) { super(w, h); this.type = type; this.flags = flags; this.format = format; }
        public LayoutParams(int w, int h, int xpos, int ypos, int type, int flags, int format) { this(w, h, type, flags, format); x = xpos; y = ypos; }
        public final void setTitle(CharSequence title) { this.title = title != null ? title : ""; }
        public final CharSequence getTitle() { return title; }
        public void setFitInsetsTypes(int types) { fitInsetsTypes = types; }
        public void setFitInsetsSides(int sides) { fitInsetsSides = sides; }
        public void setFitInsetsIgnoringVisibility(boolean ignore) { fitInsetsIgnoringVisibility = ignore; }
        public int getFitInsetsTypes() { return fitInsetsTypes; }
        public int getFitInsetsSides() { return fitInsetsSides; }
        public boolean isFitInsetsIgnoringVisibility() { return fitInsetsIgnoringVisibility; }
        public void setBlurBehindRadius(int blurBehindRadius) {}
        public void setColorMode(int colorMode) {}
        public int getColorMode() { return 0; }
        public final int copyFrom(LayoutParams o) {
            width = o.width; height = o.height; x = o.x; y = o.y; type = o.type; flags = o.flags; softInputMode = o.softInputMode;
            gravity = o.gravity; format = o.format; alpha = o.alpha; dimAmount = o.dimAmount; screenBrightness = o.screenBrightness;
            token = o.token; packageName = o.packageName; title = o.title; layoutInDisplayCutoutMode = o.layoutInDisplayCutoutMode;
            preferredRefreshRate = o.preferredRefreshRate; screenOrientation = o.screenOrientation;
            return 0xffffffff;
        }
        public static boolean mayUseInputMethod(int flags) { return true; }
        @Override public int describeContents() { return 0; }
        @Override public void writeToParcel(android.os.Parcel out, int f) { out.writeValue(this); }
    }
}
