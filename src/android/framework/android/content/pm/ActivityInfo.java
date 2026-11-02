// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.pm;

/** An activity of a package. */
public class ActivityInfo extends ComponentInfo implements android.os.Parcelable {
    public static final int CONFIG_MCC = 0x1, CONFIG_MNC = 0x2, CONFIG_LOCALE = 0x4, CONFIG_TOUCHSCREEN = 0x8,
        CONFIG_KEYBOARD = 0x10, CONFIG_KEYBOARD_HIDDEN = 0x20, CONFIG_NAVIGATION = 0x40, CONFIG_ORIENTATION = 0x80,
        CONFIG_SCREEN_LAYOUT = 0x100, CONFIG_UI_MODE = 0x200, CONFIG_SCREEN_SIZE = 0x400, CONFIG_SMALLEST_SCREEN_SIZE = 0x800,
        CONFIG_DENSITY = 0x1000, CONFIG_LAYOUT_DIRECTION = 0x2000, CONFIG_COLOR_MODE = 0x4000, CONFIG_GRAMMATICAL_GENDER = 0x8000,
        CONFIG_FONT_WEIGHT_ADJUSTMENT = 0x10000000, CONFIG_FONT_SCALE = 0x40000000;
    public static final int SCREEN_ORIENTATION_UNSPECIFIED = -1, SCREEN_ORIENTATION_LANDSCAPE = 0, SCREEN_ORIENTATION_PORTRAIT = 1,
        SCREEN_ORIENTATION_USER = 2, SCREEN_ORIENTATION_BEHIND = 3, SCREEN_ORIENTATION_SENSOR = 4, SCREEN_ORIENTATION_NOSENSOR = 5,
        SCREEN_ORIENTATION_LOCKED = 14;
    public static final int LAUNCH_MULTIPLE = 0, LAUNCH_SINGLE_TOP = 1, LAUNCH_SINGLE_TASK = 2, LAUNCH_SINGLE_INSTANCE = 3;
    public static final int COLOR_MODE_DEFAULT = 0;
    public int theme, launchMode, flags, screenOrientation = SCREEN_ORIENTATION_UNSPECIFIED, configChanges, softInputMode,
        uiOptions, colorMode;
    public String taskAffinity, targetActivity, permission, parentActivityName;

    public ActivityInfo() {}
    public ActivityInfo(ActivityInfo o) {
        super(o);
        theme = o.theme; launchMode = o.launchMode; flags = o.flags; screenOrientation = o.screenOrientation;
        configChanges = o.configChanges; softInputMode = o.softInputMode; taskAffinity = o.taskAffinity;
        targetActivity = o.targetActivity; permission = o.permission; parentActivityName = o.parentActivityName;
    }
    public final int getThemeResource() { return theme != 0 ? theme : applicationInfo != null ? applicationInfo.theme : 0; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel dest, int f) { dest.writeValue(this); }
}
