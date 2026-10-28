// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.scarletnx;

import android.Manifest;
import android.app.Activity;
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

    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
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

    /** src/gui/android.c: the saved log, as a file, to the app the user picks */
    public void share(String path) {
        share(this, path);
    }

    /** the menu's Share Log (its logs' zip) and a stopped game's (GameActivity, a text log), from the activity that
        shows them: a file of the data folder through DataProvider, another through the media store */
    static void share(Activity activity, String path) {
        String type = path.endsWith(".zip") ? "application/zip" : "text/plain";
        Uri document = DataProvider.documentUri(activity, new java.io.File(path));
        if (document != null) {
            send(activity, document, type);
            return;
        }
        MediaScannerConnection.scanFile(activity, new String[] {path}, new String[] {type}, (file, uri) -> {
            if (uri != null) send(activity, uri, type);
        });
    }

    private static void send(Activity activity, Uri uri, String type) {
        Intent send = new Intent(Intent.ACTION_SEND).setType(type).putExtra(Intent.EXTRA_STREAM, uri);
        activity.startActivity(
            Intent.createChooser(send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION), "ScarletNX log"));
    }

    /** src/gui/android.c: the data folder in the system's Files app, through DataProvider. Its viewer is the Files app
        itself where the system names none (some makers' own) */
    public void openData() {
        view(DataProvider.rootUri(this), "vnd.android.document/root");
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

    /** the Files app on uri, as the system names it, else its own where it names none (some makers' own) */
    private void view(Uri uri, String type) {
        Intent view = new Intent(Intent.ACTION_VIEW)
                          .setDataAndType(uri, type)
                          .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        try {
            startActivity(view);
        } catch (ActivityNotFoundException e) {
            try {
                startActivity(view.setPackage("com.google.android.documentsui"));
            } catch (ActivityNotFoundException again) {
                startActivity(view.setPackage("com.android.documentsui"));
            }
        }
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
