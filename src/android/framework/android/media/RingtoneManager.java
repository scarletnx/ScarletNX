// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.media;

import android.net.Uri;
import android.provider.Settings;

/** Ringtones: the default sound of each type is its settings URI. */
public class RingtoneManager {
    public static final int TYPE_RINGTONE = 1, TYPE_NOTIFICATION = 2, TYPE_ALARM = 4, TYPE_ALL = 7;
    public static final String ACTION_RINGTONE_PICKER = "android.intent.action.RINGTONE_PICKER",
        EXTRA_RINGTONE_TYPE = "android.intent.extra.ringtone.TYPE", EXTRA_RINGTONE_PICKED_URI = "android.intent.extra.ringtone.PICKED_URI";

    public static Uri getDefaultUri(int type) {
        return switch (type & TYPE_ALL) {
            case TYPE_RINGTONE -> Settings.System.DEFAULT_RINGTONE_URI;
            case TYPE_NOTIFICATION -> Settings.System.DEFAULT_NOTIFICATION_URI;
            case TYPE_ALARM -> Settings.System.DEFAULT_ALARM_ALERT_URI;
            default -> null;
        };
    }

    public static int getDefaultType(Uri defaultRingtoneUri) {
        if (Settings.System.DEFAULT_RINGTONE_URI.equals(defaultRingtoneUri)) return TYPE_RINGTONE;
        if (Settings.System.DEFAULT_NOTIFICATION_URI.equals(defaultRingtoneUri)) return TYPE_NOTIFICATION;
        if (Settings.System.DEFAULT_ALARM_ALERT_URI.equals(defaultRingtoneUri)) return TYPE_ALARM;
        return -1;
    }

    public static boolean isDefault(Uri ringtoneUri) { return getDefaultType(ringtoneUri) != -1; }
}
