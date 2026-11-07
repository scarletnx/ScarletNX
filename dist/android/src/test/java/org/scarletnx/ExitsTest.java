// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.scarletnx;

import static org.junit.Assert.assertEquals;
import static org.robolectric.Shadows.shadowOf;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.shadows.ShadowActivityManager;

/** What Android says of how a process of the app ended (Exits), for a start's mark that no process holds: the end of
    that pid after its start, never an earlier process's that had the pid, nor another pid's; none kept said so; below
    Android 11 that the system says none */
@RunWith(RobolectricTestRunner.class)
public class ExitsTest {
    private static final long STARTED = 1_767_225_600_000L;
    private ActivityManager manager;

    @Before
    public void setUp() {
        manager = (ActivityManager)RuntimeEnvironment.getApplication().getSystemService(Context.ACTIVITY_SERVICE);
    }

    private void ended(int pid, int reason, int status, long at, String description) {
        ApplicationExitInfo exit = ShadowActivityManager.ApplicationExitInfoBuilder.newBuilder()
                                       .setPid(pid)
                                       .setProcessName("org.scarletnx:game")
                                       .setReason(reason)
                                       .setStatus(status)
                                       .setImportance(400)
                                       .setRss(3_500L << 10)
                                       .setTimestamp(at)
                                       .setDescription(description)
                                       .build();
        shadowOf(manager).addApplicationExitInfo(exit);
    }

    @Test
    public void theEndOfThatProcessAfterItsStart() {
        ended(4242, ApplicationExitInfo.REASON_CRASH_NATIVE, 11, STARTED - 60_000, "an earlier process of that pid");
        ended(4243, ApplicationExitInfo.REASON_USER_REQUESTED, 0, STARTED + 1_000, "another pid");
        ended(4242, ApplicationExitInfo.REASON_LOW_MEMORY, 0, STARTED + 90_000, "lmk");
        ended(4242, ApplicationExitInfo.REASON_OTHER, 0, STARTED + 900_000, "a later process of that pid");
        assertEquals("Android says low memory (org.scarletnx:game, status 0, importance 400 cached, 3500 MiB "
                         + "resident): lmk",
                     Exits.of(manager, "org.scarletnx", 36, 4242, STARTED));
        // a mark that does not say when its process started: the newest end of that pid
        assertEquals("Android says other (org.scarletnx:game, status 0, importance 400 cached, 3500 MiB resident): "
                         + "a later process of that pid",
                     Exits.of(manager, "org.scarletnx", 36, 4242, 0));
    }

    @Test
    public void aSignalIsSaidWithItsNumberAndNoDescriptionWithNone() {
        ended(77, ApplicationExitInfo.REASON_SIGNALED, 9, STARTED + 5_000, null);
        assertEquals("Android says signaled (org.scarletnx:game, status 9, importance 400 cached, 3500 MiB resident)",
                     Exits.of(manager, "org.scarletnx", 30, 77, STARTED));
    }

    @Test
    public void noEndKeptIsSaid() {
        ended(4242, ApplicationExitInfo.REASON_CRASH, 0, STARTED - 1, "before it started");
        assertEquals("Android keeps no end of pid 4242 since it started",
                     Exits.of(manager, "org.scarletnx", 36, 4242, STARTED));
        assertEquals("Android keeps no end of pid 99", Exits.of(manager, "org.scarletnx", 36, 99, 0));
    }

    @Test
    public void belowAndroid11TheSystemSaysNone() {
        assertEquals("Android (API 29) does not say why a process ended, as Android 11 (API 30) does",
                     Exits.of(null, "org.scarletnx", 29, 4242, STARTED));
    }

    @Test
    public void reasonsAndImportancesByTheirNames() {
        assertEquals("exit self", Exits.reasonName(ApplicationExitInfo.REASON_EXIT_SELF));
        assertEquals("crash native", Exits.reasonName(ApplicationExitInfo.REASON_CRASH_NATIVE));
        assertEquals("ANR", Exits.reasonName(ApplicationExitInfo.REASON_ANR));
        assertEquals("freezer", Exits.reasonName(ApplicationExitInfo.REASON_FREEZER));
        assertEquals("package updated", Exits.reasonName(ApplicationExitInfo.REASON_PACKAGE_UPDATED));
        assertEquals("reason 17", Exits.reasonName(17)); // one a later Android may add: its number
        assertEquals("125 foreground service", Exits.importance(125));
        assertEquals("170", Exits.importance(170));
    }
}
