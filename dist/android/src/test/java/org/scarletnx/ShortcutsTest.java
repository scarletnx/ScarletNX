// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.scarletnx;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.content.Context;
import android.content.Intent;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.Bitmap;
import android.graphics.drawable.Icon;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.shadows.ShadowShortcutManager;

/** The games on the app's icon and the home screen (Shortcuts): each opens its game through PLAY, the newest first on
    the icon and the oldest past the launcher's count taken off, and the home screen's answer when it cannot add one */
@RunWith(RobolectricTestRunner.class)
@GraphicsMode(GraphicsMode.Mode.NATIVE) // a picture decodes as on a device, a file that is none to nothing
public class ShortcutsTest {
    private static final int SDK = 36;
    private Context app;
    private ShortcutManager manager;
    private ShadowShortcutManager shadow;

    @Before
    public void setUp() {
        app = RuntimeEnvironment.getApplication();
        manager = app.getSystemService(ShortcutManager.class);
        shadow = shadowOf(manager);
    }

    private static void assertPlays(String id, ShortcutInfo shortcut) {
        Intent play = shortcut.getIntent();
        assertEquals(PlayActivity.ACTION, play.getAction());
        assertEquals(PlayActivity.class.getName(), play.getComponent().getClassName());
        assertEquals(id, play.getStringExtra(PlayActivity.TITLE_ID));
    }

    /** the icon's shortcuts by their IDs, sorted: Robolectric keeps no order of its own, Shortcuts.ORDER is it */
    private List<String> listed() {
        List<String> ids = new ArrayList<>();
        for (ShortcutInfo shortcut : manager.getDynamicShortcuts()) ids.add(shortcut.getId());
        Collections.sort(ids);
        return ids;
    }

    private List<String> order() {
        return Arrays.asList(app.getSharedPreferences("shortcuts", Context.MODE_PRIVATE).getString(Shortcuts.ORDER, "")
                                 .split(","));
    }

    @Test
    public void aGamePlayedIsFirstOnTheIconAndOpensThroughPlay() {
        shadow.setMaxShortcutCountPerActivity(4);
        assertEquals("", Shortcuts.played(app, SDK, "0100ABCD00010000", "A racing game", null));
        List<ShortcutInfo> shortcuts = manager.getDynamicShortcuts();
        assertEquals(1, shortcuts.size());
        ShortcutInfo shortcut = shortcuts.get(0);
        assertEquals("0100ABCD00010000", shortcut.getId());
        assertEquals("A racing game", shortcut.getShortLabel().toString());
        assertEquals("A racing game", shortcut.getLongLabel().toString());
        assertEquals(0, shortcut.getRank());
        assertPlays("0100ABCD00010000", shortcut);
        assertEquals(Collections.singletonList("0100ABCD00010000"), shadow.getReportedShortcutsUsed());
    }

    /** past the launcher's count the oldest goes, and a game played again is first again, not twice */
    @Test
    public void theOldestPastTheLaunchersCountGoes() {
        shadow.setMaxShortcutCountPerActivity(2);
        for (String id : new String[] {"0100ABCD00010000", "0100ABCD00020000", "0100ABCD00030000"})
            assertEquals("", Shortcuts.played(app, SDK, id, "game " + id, null));
        assertEquals(Arrays.asList("0100ABCD00020000", "0100ABCD00030000"), listed());
        assertEquals(Arrays.asList("0100ABCD00030000", "0100ABCD00020000"), order());
        assertEquals("", Shortcuts.played(app, SDK, "0100ABCD00020000", "game 2", null));
        assertEquals(Arrays.asList("0100ABCD00020000", "0100ABCD00030000"), listed());
        assertEquals(Arrays.asList("0100ABCD00020000", "0100ABCD00030000"), order());
    }

    /** one the order does not know (an install before it) counts as the oldest */
    @Test
    public void oneTheOrderDoesNotKnowIsTheOldest() {
        shadow.setMaxShortcutCountPerActivity(2);
        manager.addDynamicShortcuts(Collections.singletonList(
            new ShortcutInfo.Builder(app, "0100ABCD000F0000")
                .setShortLabel("old")
                .setIntent(Shortcuts.play(app, "0100ABCD000F0000"))
                .build()));
        Shortcuts.played(app, SDK, "0100ABCD00010000", "one", null);
        Shortcuts.played(app, SDK, "0100ABCD00020000", "two", null);
        assertEquals(Arrays.asList("0100ABCD00010000", "0100ABCD00020000"), listed());
    }

    @Test
    public void anAndroidWithoutShortcutsGetsNoneAndNoError() {
        assertEquals("", Shortcuts.played(app, 24, "0100ABCD00010000", "one", null));
        assertTrue(manager.getDynamicShortcuts().isEmpty());
    }

    @Test
    public void aGameWithoutANameIsLabelledByItsId() {
        shadow.setMaxShortcutCountPerActivity(4);
        Shortcuts.played(app, SDK, "0100ABCD00010000", "", null);
        assertEquals("0100ABCD00010000", manager.getDynamicShortcuts().get(0).getShortLabel().toString());
    }

    /** the game's icon file, an image; else the app's own (a 3DS game's icon.rgba decodes to nothing) */
    @Test
    public void theGamesIconElseTheApps() throws IOException {
        File png = new File(app.getCacheDir(), "icon.png"), raw = new File(app.getCacheDir(), "icon.rgba");
        try (FileOutputStream out = new FileOutputStream(png)) {
            Bitmap picture = Bitmap.createBitmap(64, 64, Bitmap.Config.ARGB_8888);
            assertTrue(picture.compress(Bitmap.CompressFormat.PNG, 100, out));
        }
        try (FileOutputStream out = new FileOutputStream(raw)) {
            out.write(new byte[48 * 48 * 4]);
        }
        assertEquals(Icon.TYPE_BITMAP, Shortcuts.icon(app, png.getPath()).getType());
        for (String none : new String[] {raw.getPath(), new File(app.getCacheDir(), "none.jpg").getPath(), null}) {
            Icon icon = Shortcuts.icon(app, none);
            assertEquals(none, Icon.TYPE_RESOURCE, icon.getType());
            assertEquals(none, R.mipmap.ic_launcher, icon.getResId());
        }
    }

    @Test
    public void theHomeScreenIsAskedForTheGame() {
        shadow.setIsRequestPinShortcutSupported(true);
        assertEquals("", Shortcuts.pin(app, SDK, "0100ABCD00010000", "A racing game", null));
        List<ShortcutInfo> pinned = manager.getPinnedShortcuts();
        assertEquals(1, pinned.size());
        assertEquals("A racing game", pinned.get(0).getShortLabel().toString());
        assertPlays("0100ABCD00010000", pinned.get(0));
    }

    @Test
    public void aHomeScreenThatTakesNoneSaysSo() {
        shadow.setIsRequestPinShortcutSupported(false);
        assertEquals("This home screen takes no shortcuts", Shortcuts.pin(app, SDK, "0100ABCD00010000", "one", null));
        assertTrue(manager.getPinnedShortcuts().isEmpty());
    }

    @Test
    public void anAndroidBefore8NamesTheOneItNeeds() {
        assertEquals("Adding to the home screen needs Android 8 (API 26): this one is API 25",
                     Shortcuts.pin(app, 25, "0100ABCD00010000", "one", null));
        assertNotNull(manager.getPinnedShortcuts());
        assertTrue(manager.getPinnedShortcuts().isEmpty());
    }
}
