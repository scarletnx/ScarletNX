// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.scarletnx;

import android.Manifest;
import android.app.Activity;
import android.app.ActivityManager;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Insets;
import android.media.MediaScannerConnection;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.PowerManager;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.view.DisplayCutout;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import java.io.File;
import java.nio.charset.StandardCharsets;
import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

/** The menu: scarletnx-gui, as libmain.so */
public class MainActivity extends SDLActivity {
    /** past the request codes SDL counts up from 0 */
    private static final int ASKED_NOTIFICATIONS = 0x10000, ASKED_BATTERY = 0x10001, PICKED_FOLDER = 0x10002;
    /** the state: line's watch, once a process (stateWatch) */
    private static DeviceState state;
    /** the title ID a PLAY asked for (PlayActivity), until the menu takes it (takePlay) */
    private String asked;

    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        // a menu Android makes again from its saved state has its first intent again: its PLAY was taken then
        if (state == null) asked(getIntent());
        // dumps and keys are read by their path: all files from Android 11, the shared storage before it
        if (Build.VERSION.SDK_INT >= 30) {
            if (!Environment.isExternalStorageManager())
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                         Uri.parse("package:" + getPackageName())));
        } else if (Build.VERSION.SDK_INT >= 23 &&
                   checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED)
            requestPermissions(new String[] {Manifest.permission.WRITE_EXTERNAL_STORAGE}, 0);
    }

    /** src/gui/android.c: the runtime with these arguments, in the game activity's process */
    public void play(String[] args) {
        startActivity(new Intent(this, GameActivity.class).putExtra("args", args));
    }

    /** the menu as the app's icon starts it, the intent Android finds its task by: started so (PlayActivity, the
        notification's tap), it brings that task back as the icon does, never a second menu over a game, which SDL ends
        the app for */
    static Intent fromIcon(Context context) {
        return new Intent(context, MainActivity.class)
            .setAction(Intent.ACTION_MAIN)
            .addCategory(Intent.CATEGORY_LAUNCHER)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
    }

    /** a PLAY of the menu in front (PlayActivity): it pauses and resumes the menu, which then asks (takePlay) */
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        asked(intent);
    }

    /** the title ID intent asks to play, kept for the menu. None from the recent apps' list, which starts a task that
        has gone again with the intent it started with: that PLAY was played then */
    private synchronized void asked(Intent intent) {
        String id = intent != null ? intent.getStringExtra(PlayActivity.TITLE_ID) : null;
        if (id != null && (intent.getFlags() & Intent.FLAG_ACTIVITY_LAUNCHED_FROM_HISTORY) == 0) asked = id;
    }

    /** src/gui/android.c: the title ID a PLAY asked for since the last call, as UTF-8; empty for none. The menu asks as
        it starts and each time it comes in front */
    public synchronized byte[] takePlay() {
        byte[] id = asked != null ? asked.getBytes(StandardCharsets.UTF_8) : new byte[0];
        asked = null;
        return id;
    }

    /** the title ID of the game the menu started last, "" for one without: while its process runs, a PLAY of it brings
        it back (PlayActivity). The process's, which a game outlives when Android ends the menu's */
    private static volatile String playing = "";

    static String playing() {
        return playing;
    }

    private static String text(byte[] utf8) {
        return utf8 != null ? new String(utf8, StandardCharsets.UTF_8) : null;
    }

    /** src/gui/android.c: the game the menu started, its title ID ("" for none), name and icon file (null for none), as
        UTF-8: one with an ID first of the shortcuts on the app's icon (Shortcuts.played). What failed, or empty */
    public byte[] played(byte[] id, byte[] name, byte[] icon) {
        String game = id != null ? text(id) : "";
        playing = game;
        String failed =
            game.isEmpty() ? "" : Shortcuts.played(this, Build.VERSION.SDK_INT, game, text(name), text(icon));
        return failed.getBytes(StandardCharsets.UTF_8);
    }

    /** src/gui/android.c: the game's box's Add to Home Screen, its title ID, name and icon file (null for none), as
        UTF-8: empty when the home screen was asked, else why not (Shortcuts.pin) */
    public byte[] pin(byte[] id, byte[] name, byte[] icon) {
        String failed = Shortcuts.pin(this, Build.VERSION.SDK_INT, text(id), text(name), text(icon));
        return failed.getBytes(StandardCharsets.UTF_8);
    }

    /** src/gui/android.c: Back on the menu's first screen, as an app's first screen takes it: the app goes behind */
    public void toBack() {
        moveTaskToBack(true);
    }

    /** src/gui/android.c: a run's game, step and percent (-1 while unknown), the text as UTF-8, in the notification
        that keeps it going behind other apps; a run's first asks for what that needs */
    public void running(byte[] game, byte[] step, int percent) {
        String title = new String(game, StandardCharsets.UTF_8), text = new String(step, StandardCharsets.UTF_8);
        runOnUiThread(() -> {
            if (RecompileService.show(this, title, text, percent)) askBackground();
        });
    }

    /** src/gui/android.c: the run is over; result, UTF-8 when not null, stays in a notification */
    public void ran(byte[] result) {
        String text = result != null ? new String(result, StandardCharsets.UTF_8) : null;
        runOnUiThread(() -> RecompileService.end(text));
    }

    @Override
    protected void onResume() {
        super.onResume();
        RecompileService.seen(this);
    }

    /** what a run behind other apps needs, each asked once: its notification (Android 13), no battery limit
        (Android 6), then the maker's own setting (Makers) */
    private void askBackground() {
        SharedPreferences asked = getSharedPreferences("asked", MODE_PRIVATE);
        String[] maker = Makers.limits();
        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED &&
            once(asked, "notifications"))
            requestPermissions(new String[] {Manifest.permission.POST_NOTIFICATIONS}, ASKED_NOTIFICATIONS);
        else if (Build.VERSION.SDK_INT >= 23 &&
                 !((PowerManager)getSystemService(POWER_SERVICE)).isIgnoringBatteryOptimizations(getPackageName()) &&
                 once(asked, "battery")) {
            try {
                startActivityForResult(new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                                                  Uri.parse("package:" + getPackageName())),
                                       ASKED_BATTERY);
            } catch (ActivityNotFoundException e) {
                askBackground();
            }
        } else if (maker != null && once(asked, "maker"))
            Makers.ask(this, maker);
    }

    /** true the first time, which it records */
    private static boolean once(SharedPreferences asked, String what) {
        if (asked.getBoolean(what, false)) return false;
        asked.edit().putBoolean(what, true).apply();
        return true;
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] permissions, int[] results) {
        if (code == ASKED_NOTIFICATIONS)
            askBackground();
        else
            super.onRequestPermissionsResult(code, permissions, results);
    }

    @Override
    protected void onActivityResult(int code, int result, Intent data) {
        super.onActivityResult(code, result, data);
        if (code == ASKED_BATTERY) askBackground();
        if (code == PICKED_FOLDER) {
            Uri tree = result == RESULT_OK && data != null ? data.getData() : null;
            folderPicked(tree != null ? tree.toString().getBytes(StandardCharsets.UTF_8) : null);
        }
    }

    /** src/gui/android.c: the system's folder picker, SDL having none here; its answer goes to folderPicked */
    public void pickFolder() {
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICKED_FOLDER);
    }

    /** src/gui/android.c: the folder picked, its tree's URI as UTF-8; null for none */
    private static native void folderPicked(byte[] uri);

    /** src/gui/android.c: the launcher log's state: line now and at each change of it, until the process ends */
    public void stateWatch() {
        synchronized (MainActivity.class) {
            if (state != null) return;
            Context app = getApplicationContext();
            state = new DeviceState((PowerManager)app.getSystemService(POWER_SERVICE), Build.VERSION.SDK_INT,
                                    line -> stateSaid(line.getBytes(StandardCharsets.UTF_8)));
            state.watch(app);
        }
    }

    /** src/gui/android.c: a state: line, UTF-8, for the launcher log */
    private static native void stateSaid(byte[] line);

    /** src/gui/android.c: what Android says of how process pid of the app ended, started at since (ms since 1970, 0
        when unknown), as UTF-8 */
    public byte[] exitReason(int pid, long since) {
        return Exits.of(this, pid, since).getBytes(StandardCharsets.UTF_8);
    }

    /** src/gui/android.c: the same end as Exits.fields gives it, for the menu's Last Run, as UTF-8 */
    public byte[] exitFields(int pid, long since) {
        ActivityManager manager = (ActivityManager)getSystemService(ACTIVITY_SERVICE);
        return Exits.fields(manager, getPackageName(), Build.VERSION.SDK_INT, pid, since)
            .getBytes(StandardCharsets.UTF_8);
    }

    /** a stopped game's Share Log (GameActivity, a text log), from the activity that shows it: a file of the data
        folder through DataProvider, another through the media store */
    static void share(Activity activity, String path) {
        Uri document = DataProvider.documentUri(activity, new java.io.File(path));
        if (document != null) {
            send(activity, document);
            return;
        }
        MediaScannerConnection.scanFile(activity, new String[] {path}, new String[] {"text/plain"}, (file, uri) -> {
            if (uri != null) send(activity, uri);
        });
    }

    private static void send(Activity activity, Uri uri) {
        Intent send = new Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_STREAM, uri);
        activity.startActivity(
            Intent.createChooser(send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION), "ScarletNX log"));
    }

    /** src/gui/android.c: a folder in the system's Files app: one of the data folder through DataProvider, one of the
        shared storage (a dump's in Download/) through the system's own provider of it. Another is refused */
    public void openFolder(String path) {
        File folder = new File(path), data = DataProvider.folder(this);
        String shared = Environment.getExternalStorageDirectory().getAbsolutePath(), full = folder.getAbsolutePath();
        Uri uri = DataProvider.documentUri(this, folder);
        if (uri == null && full.equals(data.getAbsolutePath())) uri = DataProvider.rootUri(this);
        if (uri == null && (full.equals(shared) || full.startsWith(shared + "/")))
            uri = DocumentsContract.buildDocumentUri("com.android.externalstorage.documents",
                                                     "primary:" + full.substring(Math.min(full.length(), shared.length() + 1)));
        if (uri == null) throw new IllegalArgumentException(path + " is neither in the app's data nor in shared storage");
        view(uri, uri.equals(DataProvider.rootUri(this)) ? "vnd.android.document/root" : DocumentsContract.Document.MIME_TYPE_DIR);
    }

    /** the system's Files app on uri, the one app that reads both providers (they ask for MANAGE_DOCUMENTS): any other
        that takes a folder, which the chooser would offer (a file manager, a hex editor), opens on a place of its own.
        Without it, whatever the system names (a maker's own Files app) */
    private void view(Uri uri, String type) {
        Intent view = new Intent(Intent.ACTION_VIEW)
                          .setDataAndType(uri, type)
                          .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        for (String files : new String[] {"com.google.android.documentsui", "com.android.documentsui"}) {
            try {
                startActivity(new Intent(view).setPackage(files));
                return;
            } catch (ActivityNotFoundException e) {
                // not on this system: the next
            }
        }
        startActivity(view);
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return clearOfBars(context);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideBars(mSurface);
    }

    /** Fullscreen on, the system's bars hidden through the window's insets controller. SDL hides them again only by
        setting its flags, which does nothing while those are still set (View.setSystemUiVisibility): bars the system
        showed meanwhile, over another window or from a swipe, would stay */
    static void hideBars(View view) {
        WindowInsetsController bars =
            Build.VERSION.SDK_INT >= 30 && view != null ? view.getWindowInsetsController() : null;
        if (mFullscreenModeActive && bars != null) bars.hide(WindowInsets.Type.systemBars());
    }

    /** a surface whose safe area keeps out of the camera's cutout and of the system's bars while they show (Fullscreen
        off), not of the bars it hides nor of the gesture edges: the menu's text and a game's picture stay there */
    static SDLSurface clearOfBars(Context context) {
        return new SDLSurface(context) {
            final Runnable hide = () -> hideBars(this);

            @Override
            public WindowInsets onApplyWindowInsets(View view, WindowInsets insets) {
                WindowInsets result = super.onApplyWindowInsets(view, insets);
                if (Build.VERSION.SDK_INT >= 30) {
                    Insets safe = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                    SDLActivity.onNativeInsetsChanged(safe.left, safe.right, safe.top, safe.bottom);
                    // bars that show hide again as late as SDL's own retry would: time to reach their buttons
                    removeCallbacks(hide);
                    if (insets.isVisible(WindowInsets.Type.statusBars()) ||
                        insets.isVisible(WindowInsets.Type.navigationBars()))
                        postDelayed(hide, 2000);
                } else {
                    DisplayCutout cut = Build.VERSION.SDK_INT >= 28 ? insets.getDisplayCutout() : null;
                    SDLActivity.onNativeInsetsChanged(
                        Math.max(insets.getSystemWindowInsetLeft(), cut != null ? cut.getSafeInsetLeft() : 0),
                        Math.max(insets.getSystemWindowInsetRight(), cut != null ? cut.getSafeInsetRight() : 0),
                        Math.max(insets.getSystemWindowInsetTop(), cut != null ? cut.getSafeInsetTop() : 0),
                        Math.max(insets.getSystemWindowInsetBottom(), cut != null ? cut.getSafeInsetBottom() : 0));
                }
                return result;
            }
        };
    }
}
