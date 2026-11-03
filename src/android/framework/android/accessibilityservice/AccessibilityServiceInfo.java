// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.accessibilityservice;

import android.content.pm.PackageManager;
import android.os.Parcel;
import android.os.Parcelable;

/** An accessibility service's description. No accessibility service runs, so an app never gets one with values. */
public class AccessibilityServiceInfo implements Parcelable {
    public static final int CAPABILITY_CAN_RETRIEVE_WINDOW_CONTENT = 1, CAPABILITY_CAN_REQUEST_TOUCH_EXPLORATION = 2,
        CAPABILITY_CAN_REQUEST_FILTER_KEY_EVENTS = 8, CAPABILITY_CAN_CONTROL_MAGNIFICATION = 16,
        CAPABILITY_CAN_PERFORM_GESTURES = 32, CAPABILITY_CAN_REQUEST_FINGERPRINT_GESTURES = 64,
        CAPABILITY_CAN_TAKE_SCREENSHOT = 128;
    public static final int FEEDBACK_SPOKEN = 1, FEEDBACK_HAPTIC = 2, FEEDBACK_AUDIBLE = 4, FEEDBACK_VISUAL = 8,
        FEEDBACK_GENERIC = 16, FEEDBACK_BRAILLE = 32, FEEDBACK_ALL_MASK = -1;
    public int eventTypes, feedbackType, flags;
    public long notificationTimeout;
    public String[] packageNames;

    public AccessibilityServiceInfo() {}
    public int getCapabilities() { return 0; }
    public String loadDescription(PackageManager packageManager) { return null; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) { dest.writeValue(this); }
    public static final Parcelable.Creator<AccessibilityServiceInfo> CREATOR = new Parcelable.Creator<>() {
        @Override public AccessibilityServiceInfo createFromParcel(Parcel in) { return (AccessibilityServiceInfo) in.readValue(null); }
        @Override public AccessibilityServiceInfo[] newArray(int size) { return new AccessibilityServiceInfo[size]; }
    };
}
