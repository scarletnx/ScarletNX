// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.scarletnx;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.robolectric.Shadows.shadowOf;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ComponentName;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.Uri;
import android.os.Looper;
import android.provider.Settings;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.shadows.ShadowAlertDialog;

/** The maker's setting (Makers.ask): its Settings button tries the row's screens in turn, then the app's info */
@RunWith(RobolectricTestRunner.class)
public class MakersDialogTest {
    private Activity activity;

    @Before
    public void setUp() {
        activity = Robolectric.buildActivity(Activity.class).setup().get();
        // a screen this phone does not have is not started: ActivityNotFoundException, as on a device
        shadowOf(activity.getApplication()).checkActivities(true);
    }

    private void has(String pack, String activity) {
        shadowOf(this.activity.getPackageManager()).addActivityIfNotPresent(new ComponentName(pack, activity));
    }

    private Intent settings(String manufacturer) {
        String[] row = Makers.limits(manufacturer, manufacturer);
        Makers.ask(activity, row);
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        assertNotNull(dialog);
        assertEquals("This phone may stop a recompile behind other apps. In Settings: " + row[1],
                     shadowOf(dialog).getMessage().toString());
        dialog.getButton(DialogInterface.BUTTON_POSITIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle(); // the dialog's buttons answer through the main thread's queue
        return shadowOf(activity).getNextStartedActivity();
    }

    @Test
    public void theFirstScreenThePhoneHasOpens() {
        has("com.iqoo.secure", "com.iqoo.secure.ui.phoneoptimize.AddWhiteListActivity");
        has("com.iqoo.secure", "com.iqoo.secure.ui.phoneoptimize.BgStartUpManager");
        assertEquals(new ComponentName("com.iqoo.secure", "com.iqoo.secure.ui.phoneoptimize.AddWhiteListActivity"),
                     settings("vivo").getComponent());
    }

    @Test
    public void aScreenThisVersionLacksIsPassedOver() {
        has("com.huawei.systemmanager", "com.huawei.systemmanager.optimize.process.ProtectActivity");
        assertEquals(new ComponentName("com.huawei.systemmanager",
                                       "com.huawei.systemmanager.optimize.process.ProtectActivity"),
                     settings("huawei").getComponent());
    }

    @Test
    public void withNoneOfItsScreensTheAppsInfoOpens() {
        has("com.android.settings", "com.android.settings.applications.InstalledAppDetails");
        shadowOf(activity.getPackageManager())
            .addIntentFilterForActivity(new ComponentName("com.android.settings",
                                                          "com.android.settings.applications.InstalledAppDetails"),
                                        appDetails());
        Intent info = settings("xiaomi");
        assertEquals(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, info.getAction());
        assertEquals(Uri.parse("package:org.scarletnx"), info.getData());
    }

    private static IntentFilter appDetails() {
        IntentFilter filter = new IntentFilter(Settings.ACTION_APPLICATION_DETAILS_SETTINGS);
        filter.addCategory(Intent.CATEGORY_DEFAULT);
        filter.addDataScheme("package");
        return filter;
    }

    @Test
    public void withNoSettingsAppNothingOpensAndNothingThrows() {
        assertNull(settings("asus"));
    }

    @Test
    public void notNowOpensNothing() {
        Makers.ask(activity, Makers.limits("samsung", "samsung"));
        ShadowAlertDialog.getLatestAlertDialog().getButton(DialogInterface.BUTTON_NEGATIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertNull(shadowOf(activity).getNextStartedActivity());
    }
}
