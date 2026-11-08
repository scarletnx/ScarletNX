// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2008 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.content.res;

import android.os.LocaleList;
import java.util.Locale;

/** The device configuration resources are chosen for. */
public final class Configuration implements Comparable<Configuration> {
    public static final int ORIENTATION_UNDEFINED = 0, ORIENTATION_PORTRAIT = 1, ORIENTATION_LANDSCAPE = 2;
    public static final int UI_MODE_TYPE_MASK = 0x0f, UI_MODE_TYPE_UNDEFINED = 0, UI_MODE_TYPE_NORMAL = 1,
        UI_MODE_TYPE_DESK = 2, UI_MODE_TYPE_CAR = 3, UI_MODE_TYPE_TELEVISION = 4, UI_MODE_TYPE_APPLIANCE = 5,
        UI_MODE_TYPE_WATCH = 6, UI_MODE_TYPE_VR_HEADSET = 7;
    public static final int UI_MODE_NIGHT_MASK = 0x30, UI_MODE_NIGHT_UNDEFINED = 0, UI_MODE_NIGHT_NO = 0x10,
        UI_MODE_NIGHT_YES = 0x20;
    public static final int SCREENLAYOUT_SIZE_MASK = 0x0f, SCREENLAYOUT_SIZE_UNDEFINED = 0,
        SCREENLAYOUT_SIZE_SMALL = 1, SCREENLAYOUT_SIZE_NORMAL = 2, SCREENLAYOUT_SIZE_LARGE = 3,
        SCREENLAYOUT_SIZE_XLARGE = 4;
    public static final int SCREENLAYOUT_LONG_MASK = 0x30, SCREENLAYOUT_LONG_UNDEFINED = 0,
        SCREENLAYOUT_LONG_NO = 0x10, SCREENLAYOUT_LONG_YES = 0x20;
    public static final int SCREENLAYOUT_LAYOUTDIR_MASK = 0xc0, SCREENLAYOUT_LAYOUTDIR_SHIFT = 6,
        SCREENLAYOUT_LAYOUTDIR_UNDEFINED = 0, SCREENLAYOUT_LAYOUTDIR_LTR = 0x40, SCREENLAYOUT_LAYOUTDIR_RTL = 0x80;
    public static final int SCREENLAYOUT_ROUND_MASK = 0x300, SCREENLAYOUT_ROUND_NO = 0x100,
        SCREENLAYOUT_ROUND_YES = 0x200;
    public static final int SCREENLAYOUT_UNDEFINED = 0;
    public static final int TOUCHSCREEN_UNDEFINED = 0, TOUCHSCREEN_NOTOUCH = 1, TOUCHSCREEN_FINGER = 3;
    public static final int KEYBOARD_UNDEFINED = 0, KEYBOARD_NOKEYS = 1, KEYBOARD_QWERTY = 2, KEYBOARD_12KEY = 3;
    public static final int KEYBOARDHIDDEN_UNDEFINED = 0, KEYBOARDHIDDEN_NO = 1, KEYBOARDHIDDEN_YES = 2;
    public static final int HARDKEYBOARDHIDDEN_UNDEFINED = 0, HARDKEYBOARDHIDDEN_NO = 1, HARDKEYBOARDHIDDEN_YES = 2;
    public static final int NAVIGATION_UNDEFINED = 0, NAVIGATION_NONAV = 1, NAVIGATION_DPAD = 2,
        NAVIGATION_TRACKBALL = 3, NAVIGATION_WHEEL = 4;
    public static final int NAVIGATIONHIDDEN_UNDEFINED = 0, NAVIGATIONHIDDEN_NO = 1, NAVIGATIONHIDDEN_YES = 2;
    public static final int COLOR_MODE_UNDEFINED = 0;
    public static final int SCREEN_WIDTH_DP_UNDEFINED = 0, SCREEN_HEIGHT_DP_UNDEFINED = 0,
        SMALLEST_SCREEN_WIDTH_DP_UNDEFINED = 0, DENSITY_DPI_UNDEFINED = 0, FONT_WEIGHT_ADJUSTMENT_UNDEFINED = Integer.MAX_VALUE;
    public static final int GRAMMATICAL_GENDER_NOT_SPECIFIED = 0;

    public float fontScale;
    public int mcc, mnc;
    @Deprecated public Locale locale;
    private LocaleList locales = LocaleList.getEmptyLocaleList();
    public int screenLayout, touchscreen, keyboard, keyboardHidden, hardKeyboardHidden, navigation,
        navigationHidden, orientation, uiMode, screenWidthDp, screenHeightDp, smallestScreenWidthDp, densityDpi,
        colorMode, fontWeightAdjustment, seq;

    public Configuration() {
        setToDefaults();
    }

    public Configuration(Configuration o) {
        setTo(o);
    }

    public void setToDefaults() {
        fontScale = 1;
        mcc = mnc = 0;
        locales = LocaleList.getEmptyLocaleList();
        locale = null;
        screenLayout = touchscreen = keyboard = keyboardHidden = hardKeyboardHidden = navigation = navigationHidden =
            orientation = uiMode = screenWidthDp = screenHeightDp = smallestScreenWidthDp = densityDpi = colorMode = 0;
        fontWeightAdjustment = FONT_WEIGHT_ADJUSTMENT_UNDEFINED;
        seq = 0;
    }

    public void unset() {
        setToDefaults();
        fontScale = 0;
    }

    public void setTo(Configuration o) {
        fontScale = o.fontScale; mcc = o.mcc; mnc = o.mnc; locale = o.locale; locales = o.locales;
        screenLayout = o.screenLayout; touchscreen = o.touchscreen; keyboard = o.keyboard;
        keyboardHidden = o.keyboardHidden; hardKeyboardHidden = o.hardKeyboardHidden; navigation = o.navigation;
        navigationHidden = o.navigationHidden; orientation = o.orientation; uiMode = o.uiMode;
        screenWidthDp = o.screenWidthDp; screenHeightDp = o.screenHeightDp;
        smallestScreenWidthDp = o.smallestScreenWidthDp; densityDpi = o.densityDpi; colorMode = o.colorMode;
        fontWeightAdjustment = o.fontWeightAdjustment; seq = o.seq;
    }

    public LocaleList getLocales() {
        return locales;
    }

    public void setLocales(LocaleList l) {
        locales = l == null ? LocaleList.getEmptyLocaleList() : l;
        locale = locales.get(0);
    }

    public void setLocale(Locale l) {
        setLocales(l == null ? null : new LocaleList(l));
    }

    public int getLayoutDirection() {
        return (screenLayout & SCREENLAYOUT_LAYOUTDIR_MASK) == SCREENLAYOUT_LAYOUTDIR_RTL ? 1 : 0;
    }

    public void setLayoutDirection(Locale l) {
        int dir = l != null && android.text.TextUtils.getLayoutDirectionFromLocale(l) == 1 ? SCREENLAYOUT_LAYOUTDIR_RTL : SCREENLAYOUT_LAYOUTDIR_LTR;
        screenLayout = (screenLayout & ~SCREENLAYOUT_LAYOUTDIR_MASK) | dir;
    }

    public boolean isNightModeActive() {
        return (uiMode & UI_MODE_NIGHT_MASK) == UI_MODE_NIGHT_YES;
    }

    public boolean isLayoutSizeAtLeast(int size) {
        int cur = screenLayout & SCREENLAYOUT_SIZE_MASK;
        return cur != SCREENLAYOUT_SIZE_UNDEFINED && cur >= size;
    }

    public boolean isScreenRound() {
        return (screenLayout & SCREENLAYOUT_ROUND_MASK) == SCREENLAYOUT_ROUND_YES;
    }

    public boolean isScreenWideColorGamut() {
        return false;
    }

    public boolean isScreenHdr() {
        return false;
    }

    public int getGrammaticalGender() {
        return GRAMMATICAL_GENDER_NOT_SPECIFIED;
    }

    /** The ActivityInfo.CONFIG_* bits of the fields that differ. */
    public int diff(Configuration o) {
        int d = 0;
        if (o.fontScale > 0 && fontScale != o.fontScale) d |= 0x40000000;
        if (o.mcc != 0 && mcc != o.mcc) d |= 0x0001;
        if (o.mnc != 0 && mnc != o.mnc) d |= 0x0002;
        if (!o.locales.isEmpty() && !locales.equals(o.locales)) d |= 0x0004 | 0x2000;
        if (o.touchscreen != 0 && touchscreen != o.touchscreen) d |= 0x0008;
        if (o.keyboard != 0 && keyboard != o.keyboard) d |= 0x0010;
        if (o.keyboardHidden != 0 && keyboardHidden != o.keyboardHidden) d |= 0x0020;
        if (o.navigation != 0 && navigation != o.navigation) d |= 0x0040;
        if (o.orientation != 0 && orientation != o.orientation) d |= 0x0080;
        if (o.screenLayout != 0 && screenLayout != o.screenLayout) d |= 0x0100;
        if (o.uiMode != 0 && uiMode != o.uiMode) d |= 0x0200;
        if (o.screenWidthDp != 0 && screenWidthDp != o.screenWidthDp) d |= 0x0400;
        if (o.screenHeightDp != 0 && screenHeightDp != o.screenHeightDp) d |= 0x0400;
        if (o.smallestScreenWidthDp != 0 && smallestScreenWidthDp != o.smallestScreenWidthDp) d |= 0x0800;
        if (o.densityDpi != 0 && densityDpi != o.densityDpi) d |= 0x1000;
        if (o.fontWeightAdjustment != FONT_WEIGHT_ADJUSTMENT_UNDEFINED && fontWeightAdjustment != o.fontWeightAdjustment)
            d |= 0x10000000;
        return d;
    }

    /** The defined fields of o copied here: the bits of the ones that changed. */
    public int updateFrom(Configuration o) {
        int changed = diff(o);
        if (o.fontScale > 0) fontScale = o.fontScale;
        if (o.mcc != 0) mcc = o.mcc;
        if (o.mnc != 0) mnc = o.mnc;
        if (!o.locales.isEmpty()) setLocales(o.locales);
        if (o.touchscreen != 0) touchscreen = o.touchscreen;
        if (o.keyboard != 0) keyboard = o.keyboard;
        if (o.keyboardHidden != 0) keyboardHidden = o.keyboardHidden;
        if (o.hardKeyboardHidden != 0) hardKeyboardHidden = o.hardKeyboardHidden;
        if (o.navigation != 0) navigation = o.navigation;
        if (o.navigationHidden != 0) navigationHidden = o.navigationHidden;
        if (o.orientation != 0) orientation = o.orientation;
        if (o.screenLayout != 0) screenLayout = o.screenLayout;
        if (o.uiMode != 0) uiMode = o.uiMode;
        if (o.screenWidthDp != 0) screenWidthDp = o.screenWidthDp;
        if (o.screenHeightDp != 0) screenHeightDp = o.screenHeightDp;
        if (o.smallestScreenWidthDp != 0) smallestScreenWidthDp = o.smallestScreenWidthDp;
        if (o.densityDpi != 0) densityDpi = o.densityDpi;
        if (o.colorMode != 0) colorMode = o.colorMode;
        if (o.fontWeightAdjustment != FONT_WEIGHT_ADJUSTMENT_UNDEFINED) fontWeightAdjustment = o.fontWeightAdjustment;
        if (changed != 0) ++seq;
        return changed;
    }

    public static boolean needNewResources(int configChanges, int interestingChanges) {
        return (configChanges & (interestingChanges | 0x40000000)) != 0;
    }

    @Override
    public int compareTo(Configuration that) {
        return Integer.compare(hashCode(), that.hashCode());
    }

    public boolean equals(Configuration that) {
        return that != null && (that == this || diff(that) == 0 && that.diff(this) == 0);
    }

    @Override
    public boolean equals(Object that) {
        return that instanceof Configuration c && equals(c);
    }

    @Override
    public int hashCode() {
        return (int) (fontScale * 100) * 31 + locales.hashCode() * 17 + uiMode * 13 + screenWidthDp * 7
            + screenHeightDp * 5 + densityDpi * 3 + orientation;
    }

    @Override
    public String toString() {
        return "{" + fontScale + " " + locales + " " + screenWidthDp + "x" + screenHeightDp + "dp " + densityDpi
            + "dpi uiMode=0x" + Integer.toHexString(uiMode) + " orientation=" + orientation + "}";
    }
}
