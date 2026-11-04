// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.scarletnx;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.Application;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.os.PowerManager;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.android.controller.ServiceController;
import org.robolectric.shadows.ShadowNotificationManager;
import org.robolectric.shadows.ShadowPowerManager;

/** A run behind other apps (RecompileService): its notification while it goes and after, and the service's end */
@RunWith(RobolectricTestRunner.class)
public class RecompileServiceTest {
    private static final int RUNNING = 1, ENDED = 2;
    private Application app;
    private ShadowNotificationManager notes;
    private ServiceController<RecompileService> controller;

    @Before
    public void setUp() {
        app = RuntimeEnvironment.getApplication();
        notes = shadowOf((NotificationManager)app.getSystemService(Context.NOTIFICATION_SERVICE));
        RecompileService.end(null); // the class's state outlives a test
    }

    @After
    public void tearDown() {
        RecompileService.end(null);
        if (controller != null) controller.destroy();
    }

    /** the service Android starts for show()'s intent */
    private RecompileService started() {
        Intent intent = shadowOf(app).getNextStartedService();
        assertNotNull("the service started", intent);
        assertEquals(new ComponentName(app, RecompileService.class), intent.getComponent());
        controller = Robolectric.buildService(RecompileService.class, intent).create().startCommand(0, 1);
        return controller.get();
    }

    private static String text(Notification note, String extra) {
        CharSequence text = note.extras.getCharSequence(extra);
        return text != null ? text.toString() : null;
    }

    @Test
    public void aRunsFirstCallStartsTheServiceTheNextOnesDoNot() {
        assertTrue(RecompileService.show(app, "demo", "Extracting", -1));
        assertNotNull(shadowOf(app).getNextStartedService());
        assertFalse(RecompileService.show(app, "demo", "Compiling", 5));
        assertNull(shadowOf(app).getNextStartedService());
        RecompileService.end(null);
        assertTrue("a new run starts it again", RecompileService.show(app, "demo", "Extracting", -1));
    }

    @Test
    public void whileItGoesTheNotificationShowsTheGameTheStepAndABar() {
        RecompileService.show(app, "demo", "Extracting", -1);
        RecompileService service = started();
        Notification note = shadowOf(service).getLastForegroundNotification();
        assertEquals(RUNNING, shadowOf(service).getLastForegroundNotificationId());
        assertEquals("demo", text(note, Notification.EXTRA_TITLE));
        assertEquals("Extracting", text(note, Notification.EXTRA_TEXT));
        assertEquals("recompile", note.getChannelId());
        assertTrue("ongoing", (note.flags & Notification.FLAG_ONGOING_EVENT) != 0);
        assertEquals(100, note.extras.getInt(Notification.EXTRA_PROGRESS_MAX));
        assertTrue("a step of unknown length", note.extras.getBoolean(Notification.EXTRA_PROGRESS_INDETERMINATE));
        assertNotNull("a tap opens the menu", note.contentIntent);
        Intent menu = shadowOf(note.contentIntent).getSavedIntent();
        assertEquals(new ComponentName(app, MainActivity.class), menu.getComponent());
        assertEquals(Intent.ACTION_MAIN, menu.getAction());

        NotificationManager manager = (NotificationManager)app.getSystemService(Context.NOTIFICATION_SERVICE);
        NotificationChannel channel = manager.getNotificationChannel("recompile");
        assertNotNull(channel);
        assertEquals(NotificationManager.IMPORTANCE_LOW, channel.getImportance());

        RecompileService.show(app, "demo", "Compiling", 40);
        Notification next = notes.getNotification(RUNNING);
        assertEquals("Compiling", text(next, Notification.EXTRA_TEXT));
        assertEquals(40, next.extras.getInt(Notification.EXTRA_PROGRESS));
        assertFalse(next.extras.getBoolean(Notification.EXTRA_PROGRESS_INDETERMINATE));
    }

    @Test
    public void theProcessorStaysAwakeUntilTheRunEnds() {
        RecompileService.show(app, "demo", "Extracting", -1);
        started();
        PowerManager.WakeLock awake = ShadowPowerManager.getLatestWakeLock();
        assertNotNull(awake);
        assertTrue(awake.isHeld());
        assertEquals("ScarletNX:recompile", shadowOf(awake).getTag());
        RecompileService.end("Ready demo");
        assertFalse(awake.isHeld());
    }

    @Test
    public void anEndWithAResultLeavesANotificationSayingIt() {
        RecompileService.show(app, "demo", "Extracting", -1);
        RecompileService service = started();
        RecompileService.end("Recompiled");
        assertTrue(shadowOf(service).isStoppedBySelf());
        assertTrue(shadowOf(service).isForegroundStopped());
        Notification ended = notes.getNotification(ENDED);
        assertNotNull(ended);
        assertEquals("demo", text(ended, Notification.EXTRA_TITLE));
        assertEquals("Recompiled", text(ended, Notification.EXTRA_TEXT));
        assertFalse("not ongoing", (ended.flags & Notification.FLAG_ONGOING_EVENT) != 0);
        assertTrue("goes on a tap", (ended.flags & Notification.FLAG_AUTO_CANCEL) != 0);

        RecompileService.seen(app);
        assertNull("the menu showing takes it", notes.getNotification(ENDED));
    }

    @Test
    public void anEndWithoutAResultLeavesNothing() {
        RecompileService.show(app, "demo", "Extracting", -1);
        RecompileService service = started();
        RecompileService.end(null);
        assertTrue(shadowOf(service).isStoppedBySelf());
        assertNull(notes.getNotification(ENDED));
    }

    @Test
    public void aNewRunTakesTheLastOnesEndedNotification() {
        RecompileService.show(app, "demo", "Extracting", -1);
        started();
        RecompileService.end("Failed");
        assertNotNull(notes.getNotification(ENDED));
        RecompileService.show(app, "other", "Extracting", -1);
        assertNull(notes.getNotification(ENDED));
    }

    @Test
    public void aRunThatEndedBeforeTheServiceStartedStopsItAtOnce() {
        RecompileService.show(app, "demo", "Extracting", -1);
        RecompileService.end("Ready demo");
        RecompileService service = started();
        assertTrue(shadowOf(service).isStoppedBySelf());
        assertEquals("Ready demo", text(notes.getNotification(ENDED), Notification.EXTRA_TEXT));
        assertFalse(ShadowPowerManager.getLatestWakeLock().isHeld());
    }

    @Test
    public void dataSyncsLimitSpentStopsTheServiceAndTheRunGoesOn() {
        RecompileService.show(app, "demo", "Extracting", -1);
        RecompileService service = started();
        service.onTimeout(1, 0);
        assertTrue(shadowOf(service).isStoppedBySelf());
        assertNull(notes.getNotification(ENDED));
        assertFalse("the run goes on: its next step starts nothing",
                    RecompileService.show(app, "demo", "Compiling", 50));
    }
}
