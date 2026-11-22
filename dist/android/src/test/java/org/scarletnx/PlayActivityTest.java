// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.scarletnx;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.app.ActivityManager;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ResolveInfo;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import org.junit.After;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.shadows.ShadowToast;

/** org.scarletnx.action.PLAY (PlayActivity): what it hands the menu for a title ID, and the toast and the menu it
    shows for none, for one that is no title ID and while a game runs */
@RunWith(RobolectricTestRunner.class)
public class PlayActivityTest {
    private static final int TO_THE_MENU = Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP;

    /** the game that runs is the process's (MainActivity.playing), kept from one test to the next: none */
    @After
    public void noGamePlays() {
        Robolectric.buildActivity(MainActivity.class).get().played(new byte[0], new byte[0], null);
    }

    /** PLAY with title_id given (null: none), as adb's am start sends it: what PlayActivity started, after it ended */
    private static Intent played(String given) {
        Intent play = new Intent(PlayActivity.ACTION).setClass(RuntimeEnvironment.getApplication(), PlayActivity.class);
        if (given != null) play.putExtra(PlayActivity.TITLE_ID, given);
        PlayActivity activity = Robolectric.buildActivity(PlayActivity.class, play).create().get();
        assertTrue("it shows nothing: ended in onCreate", activity.isFinishing());
        Intent started = shadowOf(activity).getNextStartedActivity();
        assertNotNull("the menu started", started);
        assertEquals(new ComponentName(activity, MainActivity.class), started.getComponent());
        assertTrue((started.getFlags() & Intent.FLAG_ACTIVITY_NEW_TASK) != 0);
        return started;
    }

    /** the intent a launcher (AOSP's) starts the menu with: a PLAY's must be one Android takes for it, or the next tap
        on the icon over a game started from a PLAY makes a second menu, which SDL ends the app for */
    private static void asTheIcon(Intent started) {
        Intent icon = new Intent(Intent.ACTION_MAIN)
                          .addCategory(Intent.CATEGORY_LAUNCHER)
                          .setComponent(new ComponentName(RuntimeEnvironment.getApplication(), MainActivity.class));
        assertTrue(started + " is the icon's " + icon, icon.filterEquals(started));
    }

    @Test
    public void aTitleIdGoesToTheMenuInCapitals() {
        Intent started = played(" 0100abcd00010000 ");
        assertEquals("0100ABCD00010000", started.getStringExtra(PlayActivity.TITLE_ID));
        assertEquals(TO_THE_MENU, started.getFlags() & TO_THE_MENU);
        asTheIcon(started);
        assertNull("no toast for a game the menu plays", ShadowToast.getTextOfLatestToast());
    }

    @Test
    public void noTitleIdShowsTheMenuAndSaysSo() {
        Intent started = played(null);
        assertNull(started.getStringExtra(PlayActivity.TITLE_ID));
        assertEquals(0, started.getFlags() & Intent.FLAG_ACTIVITY_CLEAR_TOP);
        asTheIcon(started);
        assertEquals("No title_id: PLAY names the game by its title ID", ShadowToast.getTextOfLatestToast());
    }

    @Test
    public void oneThatIsNoTitleIdIsNamed() {
        for (String given : new String[] {"0100abcd", "0100ABCD0001000G", "0100ABCD000100000"}) {
            ShadowToast.reset();
            Intent started = played(given);
            assertNull(given, started.getStringExtra(PlayActivity.TITLE_ID));
            assertEquals("title_id " + given + " is not a title ID (16 hex digits)",
                         ShadowToast.getTextOfLatestToast());
        }
    }

    /** the menu's intent while a game runs would put a second menu over the game (CLEAR_TOP would end the game): the
        task comes back as the icon brings it, the game in front, and the toast names the game asked for */
    @Test
    public void aGameRunningIsBroughtBackAndTheOneAskedNamed() {
        Context app = RuntimeEnvironment.getApplication();
        ActivityManager.RunningAppProcessInfo game = new ActivityManager.RunningAppProcessInfo();
        game.processName = app.getPackageName() + ":game";
        shadowOf((ActivityManager)app.getSystemService(Context.ACTIVITY_SERVICE))
            .setProcesses(Collections.singletonList(game));
        assertTrue(PlayActivity.gameRuns(app));
        Intent started = played("0100ABCD00010000");
        assertNull(started.getStringExtra(PlayActivity.TITLE_ID));
        assertEquals(0, started.getFlags() & TO_THE_MENU);
        asTheIcon(started);
        assertEquals("A game is running: close it to play 0100ABCD00010000", ShadowToast.getTextOfLatestToast());
    }

    /** the game that runs, asked again (its shortcut tapped while it plays): it comes back, and no toast */
    @Test
    public void theGameThatRunsAskedAgainComesBack() {
        Context app = RuntimeEnvironment.getApplication();
        ActivityManager.RunningAppProcessInfo game = new ActivityManager.RunningAppProcessInfo();
        game.processName = app.getPackageName() + ":game";
        shadowOf((ActivityManager)app.getSystemService(Context.ACTIVITY_SERVICE))
            .setProcesses(Collections.singletonList(game));
        MainActivity menu = Robolectric.buildActivity(MainActivity.class).get();
        menu.played("0100ABCD00010000".getBytes(StandardCharsets.UTF_8), "a game".getBytes(StandardCharsets.UTF_8),
                    null);
        Intent started = played("0100abcd00010000");
        assertNull(started.getStringExtra(PlayActivity.TITLE_ID));
        assertEquals(0, started.getFlags() & TO_THE_MENU);
        asTheIcon(started);
        assertNull(ShadowToast.getTextOfLatestToast());
        // a game with no title ID started since: the one asked is another
        menu.played(new byte[0], "a game".getBytes(StandardCharsets.UTF_8), null);
        played("0100ABCD00010000");
        assertEquals("A game is running: close it to play 0100ABCD00010000", ShadowToast.getTextOfLatestToast());
    }

    @Test
    public void theMenusOwnProcessIsNoGame() {
        Context app = RuntimeEnvironment.getApplication();
        ActivityManager.RunningAppProcessInfo menu = new ActivityManager.RunningAppProcessInfo();
        menu.processName = app.getPackageName();
        shadowOf((ActivityManager)app.getSystemService(Context.ACTIVITY_SERVICE))
            .setProcesses(Collections.singletonList(menu));
        assertFalse(PlayActivity.gameRuns(app));
    }

    /** the manifest: PLAY resolves to PlayActivity, which adb and other apps may start, in a task of its own */
    @Test
    public void playIsTheActivitysExportedAction() {
        Context app = RuntimeEnvironment.getApplication();
        ResolveInfo found = app.getPackageManager().resolveActivity(
            new Intent(PlayActivity.ACTION).setPackage(app.getPackageName()), 0);
        assertNotNull("org.scarletnx.action.PLAY resolves", found);
        ActivityInfo play = found.activityInfo;
        assertEquals(PlayActivity.class.getName(), play.name);
        assertTrue("exported, for adb", play.exported);
        assertNull("android:taskAffinity=\"\": a task of its own, never the menu's", play.taskAffinity);
    }
}
