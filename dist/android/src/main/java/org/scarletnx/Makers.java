// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.scarletnx;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;
import java.util.Locale;

/** What some makers' Android adds to stop apps behind others, past Android's own battery limit: the setting to turn
    on, and the screens it is on, tried in turn, then the app's info, which every Android has */
final class Makers {
    private static final String OPPO = "Battery usage: allow background activity and auto launch.";
    private static final String HUAWEI = "App launch: Manage manually, all on.";
    private static final String TRANSSION = "Phone Master, Auto-start: on.";
    /** the maker's name in Build, the setting, then its screens: package, activity */
    private static final String[][] LIMITS = {
        {"samsung", "Battery, Background usage limits: add ScarletNX to Never sleeping apps.",
         "com.samsung.android.lool", "com.samsung.android.sm.battery.ui.BatteryActivity",
         "com.samsung.android.lool", "com.samsung.android.sm.ui.battery.BatteryActivity"},
        {"xiaomi", "Autostart on, and Battery saver: No restrictions."},
        {"oppo", OPPO},
        {"realme", OPPO},
        {"oneplus", OPPO},
        {"vivo", "Autostart on, and high background power use allowed.",
         "com.iqoo.secure", "com.iqoo.secure.ui.phoneoptimize.AddWhiteListActivity",
         "com.vivo.permissionmanager", "com.vivo.permissionmanager.activity.BgStartUpManagerActivity",
         "com.iqoo.secure", "com.iqoo.secure.ui.phoneoptimize.BgStartUpManager"},
        {"huawei", HUAWEI,
         "com.huawei.systemmanager", "com.huawei.systemmanager.startupmgr.ui.StartupNormalAppListActivity",
         "com.huawei.systemmanager", "com.huawei.systemmanager.optimize.process.ProtectActivity"},
        {"honor", HUAWEI,
         "com.hihonor.systemmanager", "com.hihonor.systemmanager.startupmgr.ui.StartupNormalAppListActivity",
         "com.huawei.systemmanager", "com.huawei.systemmanager.optimize.process.ProtectActivity"},
        {"asus", "Auto-start: allow ScarletNX.",
         "com.asus.mobilemanager", "com.asus.mobilemanager.autostart.AutoStartActivity",
         "com.asus.mobilemanager", "com.asus.mobilemanager.powersaver.PowerSaverSettings"},
        {"tecno", TRANSSION, "com.transsion.phonemaster", "com.cyin.himgr.autostart.AutoStartActivity"},
        {"infinix", TRANSSION, "com.transsion.phonemaster", "com.cyin.himgr.autostart.AutoStartActivity"},
        {"itel", TRANSSION, "com.transsion.phonemaster", "com.cyin.himgr.autostart.AutoStartActivity"},
    };

    private Makers() {}

    /** this device's maker's row, or null for one that adds no limit we know */
    static String[] limits() {
        return limits(Build.MANUFACTURER, Build.BRAND);
    }

    /** the row of the maker a device's Build names, as its manufacturer or its brand (Xiaomi makes POCO's) */
    static String[] limits(String manufacturer, String brand) {
        String maker = (manufacturer + " " + brand).toLowerCase(Locale.ROOT);
        for (String[] row : LIMITS)
            if (maker.contains(row[0])) return row;
        return null;
    }

    /** the setting, with a button to the screen it is on */
    static void ask(Activity activity, String[] row) {
        new AlertDialog.Builder(activity, android.R.style.Theme_Material_Dialog_Alert)
            .setMessage("This phone may stop a recompile behind other apps. In Settings: " + row[1])
            .setPositiveButton("Settings", (dialog, button) -> open(activity, row))
            .setNegativeButton("Not now", null)
            .show();
    }

    private static void open(Activity activity, String[] row) {
        for (int i = 2; i + 1 < row.length; i += 2) {
            try {
                activity.startActivity(new Intent().setClassName(row[i], row[i + 1]));
                return;
            } catch (ActivityNotFoundException | SecurityException e) {
                // not in this version of the maker's Android: the next
            }
        }
        try {
            activity.startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                                              Uri.parse("package:" + activity.getPackageName())));
        } catch (ActivityNotFoundException e) {
            // no settings app to show
        }
    }
}
