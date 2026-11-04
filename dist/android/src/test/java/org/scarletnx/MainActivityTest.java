// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.scarletnx;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertThrows;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.Settings;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.shadows.ShadowActivity;
import org.robolectric.shadows.ShadowAlertDialog;
import org.robolectric.shadows.ShadowBuild;
import org.robolectric.shadows.ShadowMediaScannerConnection;

/** What the menu (MainActivity) hands Android: the folders it opens in the Files app, a log to share, a game to play
    and what a run behind other apps asks for. The activity is never created: SDL's onCreate loads the native menu */
@RunWith(RobolectricTestRunner.class)
public class MainActivityTest {
    private MainActivity menu;
    private File data;
    private String shared;

    @Before
    public void setUp() {
        menu = Robolectric.buildActivity(MainActivity.class).get();
        data = DataProvider.folder(menu);
        shared = Environment.getExternalStorageDirectory().getAbsolutePath();
    }

    @After
    public void tearDown() {
        RecompileService.end(null);
    }

    private Intent opened(String path) {
        menu.openFolder(path);
        Intent view = shadowOf(menu).getNextStartedActivity();
        assertNotNull(path + " opens something", view);
        assertEquals(Intent.ACTION_VIEW, view.getAction());
        int grants = Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
        assertEquals(grants, view.getFlags() & grants);
        return view;
    }

    @Test
    public void aFolderOfTheDataOpensAsItsDocument() {
        assertTrue(new File(data, "state/share").mkdirs());
        Intent view = opened(new File(data, "state/share").getPath());
        assertEquals(DocumentsContract.buildDocumentUri("org.scarletnx.data", "data/state/share"), view.getData());
        assertEquals(Document.MIME_TYPE_DIR, view.getType());
    }

    @Test
    public void theDataFolderOpensAsTheProvidersRoot() {
        Intent view = opened(data.getPath());
        assertEquals(Uri.parse("content://org.scarletnx.data/root/data"), view.getData());
        assertEquals("vnd.android.document/root", view.getType());
    }

    @Test
    public void aSharedStorageFolderOpensInTheSystemsProviderOfIt() {
        Intent view = opened(shared + "/Download/dumps");
        assertEquals(
            DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", "primary:Download/dumps"),
            view.getData());
        assertEquals(Document.MIME_TYPE_DIR, view.getType());
        assertEquals(DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", "primary:"),
                     opened(shared).getData());
    }

    @Test
    public void anotherFolderIsRefusedAndNothingOpens() {
        for (String path : new String[] {"/system/bin", shared + "x/Download", data.getParent()}) {
            IllegalArgumentException refused =
                assertThrows(path, IllegalArgumentException.class, () -> menu.openFolder(path));
            assertEquals(path + " is neither in the app's data nor in shared storage", refused.getMessage());
        }
        assertNull(shadowOf(menu).getNextStartedActivity());
    }

    @Test
    public void pickFolderAsksTheSystemsFolderPicker() {
        menu.pickFolder();
        Intent pick = shadowOf(menu).getNextStartedActivityForResult().intent;
        assertEquals(Intent.ACTION_OPEN_DOCUMENT_TREE, pick.getAction());
    }

    private static void write(File file, String text) throws IOException {
        assertTrue(file.getParentFile().isDirectory() || file.getParentFile().mkdirs());
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        }
    }

    @Test
    public void aLogOfTheDataIsSharedThroughTheProvider() throws IOException {
        Activity game = Robolectric.buildActivity(Activity.class).setup().get();
        File log = new File(DataProvider.folder(game), "state/logs/game.log");
        write(log, "a log");
        MainActivity.share(game, log.getPath());
        Intent chooser = shadowOf(game).getNextStartedActivity();
        assertEquals(Intent.ACTION_CHOOSER, chooser.getAction());
        assertEquals("ScarletNX log", chooser.getCharSequenceExtra(Intent.EXTRA_TITLE).toString());
        Intent send = chooser.getParcelableExtra(Intent.EXTRA_INTENT);
        assertEquals(Intent.ACTION_SEND, send.getAction());
        assertEquals("text/plain", send.getType());
        assertEquals(DataProvider.documentUri(game, log), send.getParcelableExtra(Intent.EXTRA_STREAM));
        assertEquals("content://org.scarletnx.data/document/data%2Fstate%2Flogs%2Fgame.log",
                     send.getParcelableExtra(Intent.EXTRA_STREAM).toString());
        assertTrue((send.getFlags() & Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0);
    }

    @Test
    public void aLogElsewhereGoesThroughTheMediaStore() throws IOException {
        Activity game = Robolectric.buildActivity(Activity.class).setup().get();
        File log = new File(Environment.getExternalStorageDirectory(), "ScarletNX/scarletnx.log");
        write(log, "a log");
        MainActivity.share(game, log.getPath());
        assertTrue(ShadowMediaScannerConnection.getSavedPaths().toString(),
                   ShadowMediaScannerConnection.getSavedPaths().contains(log.getPath()));
        assertTrue(ShadowMediaScannerConnection.getSavedMimeTypes().contains("text/plain"));
        // the share sheet waits for the media store's URI
        assertNull(shadowOf(game).getNextStartedActivity());
    }

    @Test
    public void theMenusArgumentsReachTheGame() {
        String[] args = {"--game=recompiled/demo", "--fullscreen=1"};
        menu.play(args);
        Intent play = shadowOf(menu).getNextStartedActivity();
        assertEquals(new ComponentName(menu, GameActivity.class), play.getComponent());
        assertArrayEquals(args, Robolectric.buildActivity(GameActivity.class, play).get().getArguments());
    }

    @Test
    public void aGameStartedWithoutArgumentsGetsNone() {
        assertArrayEquals(new String[0], Robolectric.buildActivity(GameActivity.class).get().getArguments());
    }

    /** what the menu started for a result since the last call, but the screen requestPermissions starts */
    private List<ShadowActivity.IntentForResult> asked() {
        List<ShadowActivity.IntentForResult> asked = new ArrayList<>();
        for (ShadowActivity.IntentForResult next; (next = shadowOf(menu).getNextStartedActivityForResult()) != null;)
            if (!"android.content.pm.action.REQUEST_PERMISSIONS".equals(next.intent.getAction())) asked.add(next);
        return asked;
    }

    /** a run's first step asks for its notification, then the battery limit, then the maker's: each once, ever */
    @Test
    public void aRunAsksWhatRunningBehindNeedsEachOnce() {
        ShadowBuild.setManufacturer("samsung");
        ShadowBuild.setBrand("samsung");
        ShadowActivity shadow = shadowOf(menu);
        byte[] game = "demo".getBytes(StandardCharsets.UTF_8), step = "Extracting".getBytes(StandardCharsets.UTF_8);
        menu.running(game, step, -1);

        ShadowActivity.PermissionsRequest notifications = shadow.getLastRequestedPermission();
        assertNotNull("the notification asked", notifications);
        assertArrayEquals(new String[] {Manifest.permission.POST_NOTIFICATIONS}, notifications.requestedPermissions);
        menu.onRequestPermissionsResult(notifications.requestCode, notifications.requestedPermissions,
                                        new int[] {PackageManager.PERMISSION_DENIED});

        List<ShadowActivity.IntentForResult> asked = asked();
        assertEquals(1, asked.size());
        ShadowActivity.IntentForResult battery = asked.get(0);
        assertEquals(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, battery.intent.getAction());
        assertEquals(Uri.parse("package:org.scarletnx"), battery.intent.getData());
        menu.onActivityResult(battery.requestCode, Activity.RESULT_CANCELED, null);

        AlertDialog maker = ShadowAlertDialog.getLatestAlertDialog();
        assertNotNull("the maker's setting named", maker);
        assertEquals("This phone may stop a recompile behind other apps. In Settings: Battery, Background usage "
                         + "limits: add ScarletNX to Never sleeping apps.",
                     shadowOf(maker).getMessage().toString());
        maker.dismiss();

        // the next run asks nothing more
        RecompileService.end(null);
        ShadowAlertDialog.reset();
        menu.running(game, step, 50);
        assertEquals(notifications, shadow.getLastRequestedPermission());
        assertEquals(0, asked().size());
        assertNull(ShadowAlertDialog.getLatestAlertDialog());
    }

    /** a run going on asks nothing: the notification it updates is the run's first's */
    @Test
    public void aRunsNextStepsAskNothing() {
        byte[] game = "demo".getBytes(StandardCharsets.UTF_8), step = "Extracting".getBytes(StandardCharsets.UTF_8);
        menu.running(game, step, -1);
        ShadowActivity.PermissionsRequest first = shadowOf(menu).getLastRequestedPermission();
        assertNotNull(first);
        asked();
        menu.running(game, "Compiling".getBytes(StandardCharsets.UTF_8), 10);
        assertEquals(first, shadowOf(menu).getLastRequestedPermission());
        assertEquals(0, asked().size());
    }
}
